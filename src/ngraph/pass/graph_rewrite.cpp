//*****************************************************************************
// Copyright 2017-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include <algorithm>
#include <iostream>
#include <regex>
#include <unordered_set>
#include <vector>

#include "graph_rewrite.hpp"
#include "ngraph/log.hpp"

using namespace std;
using namespace ngraph;

// GraphRewrite algorithm:
// GraphRewrite processes an input graph in an topological order(i.e. args before users)
// Given the following graph:          Abs2
//                                   /       \
//                         Constant1         Add4 - Result5
//                                   \      /
//                                    Neg3
//
// The topological order would be : `Constant1`, `Abs2`, `Neg3`, `Add4`, `Result5`
// Note, `Abs2` comes before `Neg3` as `Abs2`'s id = 2 is *less* than `Neg3`'s one (id = 3)
// Next, GraphRewrite will invoke matchers registered in an order registered in a c-tor
// i.e. if a c-tor calls `construct_m1()`; `construct_m2()`; `construct_m3()`;
// Matchers will be called as follows: `m1`, `m2`, `m3`
// Matchers should only replace nodes in the graph that come before the current root
// node in the topological order. For example, if Matcher matches Neg3, it should only
// replace nodes `Abs2` and `Constant1` if needed
// This gives Matchers a nice cascading property. For example, if m1 folds `Abs2(Constant1)`
// and `m2` folds `Neg3(Constant1)` when `m3` is called on `Add4` it will discover that
// both `Abs2` and `Neg3` were already replaced by constants, so `Add4` will also be folded into one.
// If any Matcher succeeds the rest of the matchers will **not** be called.
// E.g. if `m1` succeeds and replaces `Abs2` with a new constant, nor `m2` or `m3` will be called
// However, sometimes, you will need more than one fusion occur on the same node.
// In this case, you should be able to request another pass of GraphRewrite.
// To request another pass, you will need to register fusions in a callback:
// i.e. you will need to pass `this` into a callback and then call `this->construct_X`
// This will schedule another pass of GraphRewrite with the following fusion.
// This approach should only be used if you are either:
// a) need more than one fusion occur on the same node
// b) you are modifying nodes after the current node in the topological order
// c) there's no linear order of fusions which will give
//    the correct final fusion. i.e. the same fusion needs to occur before and after some other fusion

bool pass::GraphRewrite::run_on_function(shared_ptr<Function> f)
{
    bool rewritten = false;
    const size_t NUM_TRIES = 10;
    size_t tries = NUM_TRIES;
    vector<MatchClosure> original_matchers{m_matchers};
    do
    {
        rewritten = false;
        // m_matchers may contain newly constructed matchers for matchers
        // that need multiple passes. See comments above.
        vector<MatchClosure> run_matchers{m_matchers};
        m_matchers.clear();
        for (auto node : f->get_ordered_ops())
        {
            for (auto& mc : run_matchers)
            {
                NGRAPH_DEBUG << "Running matcher " << mc.matcher->get_name() << "("
                             << mc.matcher->get_pattern()->get_name() << ") on "
                             << node->get_name();
                if (mc.matcher->match(node))
                {
                    NGRAPH_DEBUG << "Matcher " << mc.matcher << mc.matcher->get_name()
                                 << " matched " << node->get_name();
                    if (mc.callback(*mc.matcher.get()))
                    {
                        rewritten = true;
                        break;
                    }
                }
            }
        }

    } while (rewritten && m_matchers.size() > 0 && tries--);

    m_matchers.assign(original_matchers.begin(), original_matchers.end());
    return (NUM_TRIES - tries) > 1; //this means a graph was transformed
}

static vector<regex> initialize_fusion_regexes()
{
    const char* cnsf = getenv("NGRAPH_DISABLED_FUSIONS");
    vector<regex> regexes;
    if (cnsf)
    {
        const string nsf = cnsf;
        const auto sregexes = split(nsf, ';');

        transform(sregexes.begin(),
                  sregexes.end(),
                  back_inserter(regexes),
                  [](const string& c) -> regex { return regex(c); });
    }
    return regexes;
}

bool pass::GraphRewrite::is_enabled(const shared_ptr<pattern::Matcher>& m) const
{
    //note, regexes are static to avoid re-initialization
    static const auto regexes = initialize_fusion_regexes();

    for (const auto& regex : regexes)
    {
        if (regex_match(m->get_name(), regex))
        {
            NGRAPH_DEBUG << "Disabling matcher " << m->get_name();
            return false;
        }
    }

    return true;
}

void pass::GraphRewrite::add_matcher(const shared_ptr<pattern::Matcher>& m,
                                     const graph_rewrite_callback& callback)
{
    if (is_enabled(m))
    {
        m_matchers.push_back({m, callback});
    }
}

void pass::RecurrentGraphRewrite::add_matcher(
    const std::shared_ptr<pattern::RecurrentMatcher>& m,
    const ngraph::recurrent_graph_rewrite_callback& callback)
{
    m_matchers.push_back({m, callback});
}

bool pass::RecurrentGraphRewrite::run_on_function(shared_ptr<Function> f)
{
    bool changed = false;
    size_t i = 0;
    do
    {
        for (auto node : f->get_ops())
        {
            for (auto& mc : m_matchers)
            {
                NGRAPH_DEBUG << "Running matcher " << mc.matcher << " on " << node->get_name();
                if (mc.matcher->match(node))
                {
                    NGRAPH_DEBUG << "Matcher " << mc.matcher << " matched " << node->get_name();
                    if (mc.callback(*mc.matcher.get()))
                    {
                        changed = true;
                        goto next_fusion;
                    }
                }
            }
        }
    next_fusion:
        i++;
    } while (changed && i < m_num_iters);
    return changed;
}
