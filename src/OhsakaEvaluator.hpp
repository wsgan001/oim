/*
 Copyright (c) 2015 Siyu Lei, Silviu Maniu, Luyi Mo (University of Hong Kong)

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

//  Implements the algorithm described in
//  Ohsaka et al. Fast and Accurate Influence Maximization on Large Networks
//    with Pruned Monte-Carlo Simulation. Proc. AAAI. 2014


#ifndef oim_OhsakaEvaluator_h
#define oim_OhsakaEvaluator_h

#include <random>
#include <memory>
#include <algorithm>
#include <stack>
#include <queue>

#include "common.hpp"
#include "Evaluator.hpp"
#include "InfluenceDistribution.hpp"
#include "SingleInfluence.hpp"
#include "Graph.hpp"


typedef std::unordered_map<unode_int, unode_int> cc_map;
typedef std::unordered_map<unsigned int, std::unordered_set<unode_int>>
    cc_node_map;

/**
  Implementation of PMC (MC method that is robust to edge probabilities not
  summing to 1).
*/
class OhsakaEvaluator : public Evaluator {
 private:
  // Structures containing the CCs detected in each round
  std::vector<cc_map> cc;
  std::vector<cc_node_map> cc_list;
  std::vector<Graph> graphs;
  std::vector<std::unordered_set<unode_int>> A;
  std::vector<std::unordered_set<unode_int>> D;
  std::vector<unode_int> h;
  std::vector<std::unordered_map<unode_int, bool>> latest;
  std::vector<std::unordered_map<unode_int, float>> delta;
  unsigned int R_;  // Number of MC simulations

  // For Tarjan's algorithm
  std::unordered_map<unode_int, unode_int> lowlink;
  std::unordered_map<unode_int, unode_int> index;
  std::unordered_map<unode_int, unode_int> pred;
  std::unordered_set<unode_int> visited;
  std::stack<unode_int> vis_stack;
  unode_int cur_index;

  // random devices
  std::random_device rd;
  std::mt19937 gen;
  std::uniform_real_distribution<> dist;

public:
  OhsakaEvaluator(unsigned int R) : R_(R), gen(rd()), dist(0, 1) {};

  std::unordered_set<unode_int> select(
      const Graph& graph, Sampler& sampler,
      const std::unordered_set<unode_int>& activated, unsigned int k) {

    A.clear(); D.clear(); h.clear(); latest.clear(); delta.clear();
    graphs.clear(); cc.clear(); cc_list.clear();
    std::unordered_set<unode_int> set;

    // Sample the graphs and create DAGs and supporting structures
    for (unsigned int i = 0; i < R_; i++) {
      tarjan(sampler, graph); // samples and creates the DAG at the same time
      unode_int max_node = 0;
      unode_int max_val = 0;
      std::unordered_set<unode_int> cur_A;
      std::unordered_set<unode_int> cur_D;
      A.push_back(cur_A); D.push_back(cur_D);
      //find the highest degree node (only outgoing)
      for (auto node : graphs[i].get_nodes()) {
        unode_int deg = 0;
        if (graphs[i].has_neighbours(node))
          deg = graphs[i].get_neighbours(node).size();
        if (deg >= max_val) {
          max_val = deg;
          max_node = node;
        }
      }
      // Removing activated nodes
      for (auto node : activated) {
        for (auto cc : cc_list[i]) {
          if (cc.second.find(node) != cc.second.end())
            cc.second.erase(node);
        }
      }
      // Compute the set of ancestors and descendants
      h.push_back(max_node);
      bfs(h[i], i,D[i]);
      for (auto node : graphs[i].get_nodes())
        if (node != h[i] && (D[i].find(node) == D[i].end()))
          bfs(node, i, A[i], true, h[i]);
      std::unordered_map<unode_int, bool> cur_latest;
      std::unordered_map<unode_int, float> cur_delta;
      for (auto node : graphs[i].get_nodes()) {
        cur_latest[node] = false;
        cur_delta[node] = 0.0;
      }
      delta.push_back(cur_delta);
      latest.push_back(cur_latest);
    }
    // Main loop for computing the seed set
    while (set.size() < k) {
      unode_int t = 0;
      float val_max = 0;
      for (auto v : graph.get_nodes()) {
        if (activated.find(v) == activated.end()) {
          float tot_val = 0;
          for (unsigned int i = 0; i < R_; i++)
            tot_val += gain(i, v, set);
          tot_val = tot_val / (float)R_;
          if (tot_val >= val_max) {
            val_max = tot_val;
            t = v;
          }
        }
      }
      set.insert(t);
      for (unsigned int i = 0; i < R_; i++) update_dag(i, t);
    }
    return set;
  }

 private:
  void tarjan(Sampler& sampler, const Graph& graph) {
    lowlink.clear(); index.clear(); visited.clear();
    pred.clear();
    cur_index = 0;
    unsigned int cur_num_cc = 0;
    cc_map cur_cc; cc_node_map cur_cc_list;
    for (auto node : graph.get_nodes()) {
      if (index.find(node) == index.end()) {
        pred[node] = node;
        scc(node, sampler, graph, cur_cc, cur_cc_list, cur_num_cc);
      }
    }
    cc.push_back(cur_cc);
    cc_list.push_back(cur_cc_list);
    Graph dag;
    // Create the DAG (actually, most probably a spanning tree...)
    for (auto val : pred) {
      dag.add_node(val.first);
      if (cur_cc[val.second] != cur_cc[val.first]) {
        std::unique_ptr<InfluenceDistribution>
            dst_one(new SingleInfluence(1.0));
        dag.add_edge(cur_cc[val.second], cur_cc[val.first], std::move(dst_one));
      }
    }
    graphs.push_back(dag);
    while (vis_stack.size() != 0) vis_stack.pop();
  }

  void scc(unode_int node, Sampler& sampler, const Graph& graph,
           cc_map& cur_cc, cc_node_map& cur_cc_list, unsigned int& cur_num_cc) {
    index[node] = cur_index;
    lowlink[node] = cur_index;
    cur_index++;
    visited.insert(node);
    vis_stack.push(node);
    // Recursive loop for finding cycles
    if (graph.has_neighbours(node)) {
      for (auto edge : graph.get_neighbours(node)) {
        double dice_dst = edge.dist->sample(sampler.get_type());
        double dice = dist(gen);
        if (dice < dice_dst) {
          if (index.find(edge.target) == index.end()) {
            pred[edge.target] = node;
            scc(edge.target, sampler, graph, cur_cc, cur_cc_list, cur_num_cc);
            lowlink[node] = std::min(lowlink[node], lowlink[edge.target]);
          } else {
            lowlink[node] = std::min(lowlink[node],index[edge.target]);
          }
        }
      }
    }
    // If found the root, create SCC
    if (lowlink[node] == index[node]) {
      unode_int cur_node;
      do {
        cur_node = vis_stack.top();
        vis_stack.pop();
        visited.erase(cur_node);
        cur_cc[cur_node] = cur_num_cc;
        cur_cc_list[cur_num_cc].insert(cur_node);
      } while(cur_node != node);
      cur_num_cc++;
    }
  }

  void bfs(unode_int node, int i, std::unordered_set<unode_int>& col,
           bool to=false, unode_int to_node=0) {
    std::queue<unode_int> q;
    std::unordered_set<unode_int> visited;
    q.push(node);
    visited.insert(node);
    while (q.size() != 0) {
      unode_int cur_node = q.front();
      q.pop();
      visited.insert(cur_node);
      if (graphs[i].has_neighbours(cur_node)) {
        for (auto neigh : graphs[i].get_neighbours(cur_node)) {
          if(visited.find(neigh.target) == visited.end()) {
            q.push(neigh.target);
            visited.insert(neigh.target);
            if(!to) {
              col.insert(neigh.target);
            } else if (neigh.target == to_node) {
              col.insert(node);
              return;
            }
          }
        }
      }
    }
  }

  float gain(int i, unode_int node,
             std::unordered_set<unode_int>& set) {
    unode_int v = cc[i][node];
    if (!graphs[i].has_node(v)) return 0.0;
    if (latest[i][v]) return delta[i][v];
    latest[i][v] = true;
    // If part of the ancestors of h[i], prune
    if ((A[i].find(v) != A[i].end()) && (set.size() == 0)) { // CHANGED
      delta[i][v] = gain(i,*cc_list[i][h[i]].begin(),set);
      // Still unclear if it's supposed to loop over all nodes in the SCC
    } else { //otherwise, main BFS loop
      delta[i][v] = 0.0;
    }

    std::queue<unode_int> Q;
    std::unordered_set<unode_int> X;
    Q.push(v); X.insert(v);
    while(Q.size()!=0){
      unode_int u = Q.front();
      Q.pop();
      if((A[i].find(v)!=A[i].end())&&(D[i].find(u)!=D[i].end())&&\
         (set.size()==0))
        continue;
      delta[i][v] = delta[i][v] + (float)(cc_list[i][u].size());
      if(graphs[i].has_neighbours(u))
        for(auto neigh:graphs[i].get_neighbours(u)){
          if((X.find(neigh.target)==X.end())&&\
             graphs[i].has_node(neigh.target)){
            Q.push(neigh.target);
            X.insert(neigh.target);
          }
        }
    }
    return delta[i][v];
  }

  void update_dag(int i, unode_int node){
    unode_int t = cc[i][node];
    std::unordered_set<unode_int> desc;
    bfs(t,i,desc);
    for(auto v:graphs[i].get_nodes()){
      if(latest[i][v]){
        for(auto u:desc){
          std::unordered_set<unode_int> reach;
          bfs(v,i,reach,true,u);
          if(reach.size()!=0){
            latest[i][v] = false;
            break;
          }
        }
      }
    }
    for(auto node:desc) graphs[i].remove_node(node);
  }

};

#endif
