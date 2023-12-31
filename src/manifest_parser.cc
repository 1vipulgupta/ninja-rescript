// Copyright 2011 Google Inc. All Rights Reserved.
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

#include "manifest_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "graph.h"
#include "state.h"
#include "util.h"
#include "version.h"
#include "metrics.h"
#include "debug_flags.h"
#include "disk_interface.h"
ManifestParser::ManifestParser(State* state, DiskInterface* file_reader,
                               ManifestParserOptions options)
    : Parser(state, file_reader),
      options_(options), quiet_(false) {
  env_ = &state->bindings_;
}

bool ManifestParser::Parse(const string& filename, const string& input,
                           string* err) {
  METRIC_RECORD("parse build.ninja");                             
  lexer_.Start(filename, input);

  for (;;) {
    Lexer::Token token = lexer_.ReadToken();
    switch (token) {
/*      
    case Lexer::POOL:
      if (!ParsePool(err))
        return false;
      break;
*/      
    case Lexer::BUILD:
      if (!ParseEdge(err))
        return false;
      break;
    case Lexer::RULE:
      if (!ParseRule(err))
        return false;
      break;
    case Lexer::DEFAULT:
      if (!ParseDefault(err))
        return false;
      break;
    case Lexer::IDENT: {
      lexer_.UnreadToken();
      string name;
      EvalString let_value;
      if(!lexer_.ReadIdent(&name)){
        return lexer_.Error("expected variable name",err);
      }
      if(lexer_.PeekToken(Lexer::COLON_EQUAL)){
        string value = env_->LookupVariable(name);
        char buf[20]; 
        if(!lexer_.ReadVarValue(&let_value,err)) return false;
        string file = let_value.Evaluate(env_);
        TimeStamp mtime = 0;
        if(!file.empty()) mtime = file_reader_->Stat(file, err); 
        snprintf(buf,sizeof(buf),"-" "%" PRIx64, mtime);        
        value.append(buf);
        env_->AddBinding(name,value);
      } else if(lexer_.PeekToken(Lexer::EQUALS)){
        if(!lexer_.ReadVarValue(&let_value,err)) return false;
        if (name == "rescript") {
          state_ -> rescript_mode_ = true;
        } else if(name == "cleaner") {
          state_ -> cleaner =  let_value.Evaluate(env_);
        } else {
          string value = let_value.Evaluate(env_);
          env_->AddBinding(name, value);
        }
      } else {
        return lexer_.Error("expected = or := ", err);
      }            
      break;
    }
    case Lexer::INCLUDE:
      if (!ParseFileInclude(false, err))
        return false;
      break;
    case Lexer::SUBNINJA:
      if (!ParseFileInclude(true, err))
        return false;
      break;
    case Lexer::ERROR: {
      return lexer_.Error(lexer_.DescribeLastError(), err);
    }
    case Lexer::TEOF:
      return true;
    case Lexer::NEWLINE:
      break;
    default:
      return lexer_.Error(string("unexpected ") + Lexer::TokenName(token),
                          err);
    }
  }
  return false;  // not reached
}


bool ManifestParser::ParsePool(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected pool name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (state_->LookupPool(name) != NULL)
    return lexer_.Error("duplicate pool '" + name + "'", err);

  int depth = -1;

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString value;
    if (!ParseLet(&key, &value, err))
      return false;

    if (key == "depth") {
      string depth_string = value.Evaluate(env_);
      depth = atol(depth_string.c_str());
      if (depth < 0)
        return lexer_.Error("invalid pool depth", err);
    } else {
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (depth < 0)
    return lexer_.Error("expected 'depth =' line", err);

  state_->AddPool(new Pool(name, depth));
  return true;
}


bool ManifestParser::ParseRule(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected rule name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (env_->LookupRuleCurrentScope(name) != NULL)
    return lexer_.Error("duplicate rule '" + name + "'", err);

  Rule* rule = new Rule(name);  // XXX scoped_ptr

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString value;
    if (!ParseLet(&key, &value, err))
      return false;

    if (Rule::IsReservedBinding(key)) {
      rule->AddBinding(key, value);
    } else {
      // Die on other keyvals for now; revisit if we want to add a
      // scope here.
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (rule->bindings_["rspfile"].empty() !=
      rule->bindings_["rspfile_content"].empty()) {
    return lexer_.Error("rspfile and rspfile_content need to be "
                        "both specified", err);
  }

  if (rule->bindings_["command"].empty())
    return lexer_.Error("expected 'command =' line", err);

  env_->AddRule(rule);
  return true;
}

bool ManifestParser::ParseLet(string* key, EvalString* value, string* err) {
  if (!lexer_.ReadIdent(key))
    return lexer_.Error("expected variable name", err);
  if (!ExpectToken(Lexer::EQUALS, err))
    return false;
  if (!lexer_.ReadVarValue(value, err))
    return false;
  return true;
}

bool ManifestParser::ParseDefault(string* err) {
  EvalString eval;
  if (!lexer_.ReadPath(&eval, err))
    return false;
  if (eval.empty())
    return lexer_.Error("expected target name", err);

  do {
    string path = eval.Evaluate(env_);
    string path_err;
    uint64_t slash_bits;  // Unused because this only does lookup.
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer_.Error(path_err, err);
    if (!state_->AddDefault(path, &path_err))
      return lexer_.Error(path_err, err);

    eval.Clear();
    if (!lexer_.ReadPath(&eval, err))
      return false;
  } while (!eval.empty());

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  return true;
}

bool ManifestParser::ParseEdge(string* err) {
  METRIC_RECORD("parse Edge");
  vector<EvalString> ins, outs;

  {
    EvalString out;
    if (!lexer_.ReadPath(&out, err))
      return false;
    while (!out.empty()) {
      outs.push_back(out);

      out.Clear();
      if (!lexer_.ReadPath(&out, err))
        return false;
    }
  }

  // Add all implicit outs, counting how many as we go.
  int implicit_outs = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    for (;;) {
      EvalString out;
      if (!lexer_.ReadPath(&out, err))
        return err;
      if (out.empty())
        break;
      outs.push_back(out);
      ++implicit_outs;
    }
  }

  if (outs.empty())
    return lexer_.Error("expected path", err);

  if (!ExpectToken(Lexer::COLON, err))
    return false;

  string rule_name;
  if (!lexer_.ReadIdent(&rule_name))
    return lexer_.Error("expected build command name", err);

  const Rule* rule = env_->LookupRule(rule_name);
  if (!rule)
    return lexer_.Error("unknown build rule '" + rule_name + "'", err);

  for (;;) {
    // XXX should we require one path here?
    EvalString in;
    if (!lexer_.ReadPath(&in, err))
      return false;
    if (in.empty())
      break;
    ins.push_back(in);
  }

  // Add all implicit deps, counting how many as we go.
  int implicit = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    for (;;) {
      EvalString in;
      if (!lexer_.ReadPath(&in, err))
        return err;
      if (in.empty())
        break;
      ins.push_back(in);
      ++implicit;
    }
  }

  // Add all order-only deps, counting how many as we go.
  int order_only = 0;
  if (lexer_.PeekToken(Lexer::PIPE2)) {
    for (;;) {
      EvalString in;
      if (!lexer_.ReadPath(&in, err))
        return false;
      if (in.empty())
        break;
      ins.push_back(in);
      ++order_only;
    }
  }

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  // Bindings on edges are rare, so allocate per-edge envs only when needed.
  bool has_indent_token = lexer_.PeekToken(Lexer::INDENT);
  BindingEnv* env = has_indent_token ? new BindingEnv(env_) : env_;
  while (has_indent_token) {
    string key;
    EvalString val;
    if (!ParseLet(&key, &val, err))
      return false;

    env->AddBinding(key, val.Evaluate(env_));
    has_indent_token = lexer_.PeekToken(Lexer::INDENT);
  }

#if 1 
  if (IgnoreGenerator() && rule->GetBinding("generator")){
    return true;
  }
#endif

  Edge* edge = state_->AddEdge(rule);
  edge->env_ = env;
  edge->outputs_.reserve(outs.size());
  for (size_t i = 0, e = outs.size(); i != e; ++i) {
    string path = outs[i].Evaluate(env);
    string path_err;
    uint64_t slash_bits;
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer_.Error(path_err, err);
    if (!state_->AddOut(edge, path, slash_bits)) {
        lexer_.Error("multiple rules generate " + path, err);
        return false;    
    }
  }

  edge->implicit_outs_ = implicit_outs;
  size_t inputs_reserve_size = ins.size();
  string dyndep; 
  bool hasDynDep = rule -> GetBinding("dyndep");
  if (hasDynDep) {
    implicit++;
    inputs_reserve_size++;
  }

  edge->inputs_.reserve(inputs_reserve_size);
  uint64_t dyndep_slash_bits = 0;

  for (vector<EvalString>::iterator i = ins.begin(); i != ins.end(); ++i) {
    string path = i->Evaluate(env);
    string path_err;
    uint64_t slash_bits;
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer_.Error(path_err, err);
    state_->AddIn(edge, path, slash_bits);
    if(hasDynDep && i == ins.begin()){
      dyndep = path.substr(0,path.find_last_of('.')) + ".d";
      dyndep_slash_bits = slash_bits;
    }
  }
  edge->implicit_deps_ = implicit;
  edge->order_only_deps_ = order_only;

  // Lookup, validate, and save any dyndep binding.  It will be used later
  // to load generated dependency information dynamically, but it must
  // be one of our manifest-specified inputs.
  if (hasDynDep) {
    edge->dyndep_ = state_->GetNode(dyndep, dyndep_slash_bits);
    edge->dyndep_->set_dyndep_pending(true);
    state_->AddIn(edge, dyndep, dyndep_slash_bits);
  }

  return true;
}

bool ManifestParser::ParseFileInclude(bool new_scope, string* err) {
  EvalString eval;
  if (!lexer_.ReadPath(&eval, err))
    return false;
  string path = eval.Evaluate(env_);

  ManifestParser subparser(state_, file_reader_, options_);
  if (new_scope) {
    subparser.env_ = new BindingEnv(env_);
  } else {
    subparser.env_ = env_;
  }

  if (!subparser.Load(path, err, &lexer_))
    return false;

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  return true;
}
