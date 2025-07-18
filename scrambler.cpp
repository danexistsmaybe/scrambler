/* -*- C++ -*-
 *
 * A simple scrambler for SMT-LIB 2.6 scripts
 *
 * Copyright (C) 2021 Jochen Hoenicke
 * Copyright (C) 2018-2019 Aina Niemetz
 * Copyright (C) 2015-2018 Tjark Weber
 * Copyright (C) 2011 Alberto Griggio
 *
 * Copyright (C) 2011 Alberto Griggio
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "scrambler.h"
#include <sstream>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <iostream>
#include <string.h>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <assert.h>
#include <ctype.h>
#include <stack>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <vector>
#include <ranges>
#include <algorithm>


////////////////////////////////////////////////////////////////////////////////

/*
 * pseudo-random number generator
 */
uint64_t seed;
const uint64_t a = 25214903917ULL;
const uint64_t c = 11U;
const uint64_t mask = ~(2ULL << 48);

void set_seed(int s)
{
    seed = s;
}

size_t next_rand_int(size_t upper_bound)
{
    seed = ((seed * a) + c) & mask;
    return (size_t)(seed >> 16U) % upper_bound;
}

////////////////////////////////////////////////////////////////////////////////

/* The different modes for term_annot:
 *  all:     keep all term annotations.
 *  pattern: keep pattern annotations, strip named annotations.
 *  none:    remove all term annotations.
 */
enum annotation_mode { none, pattern, all };


/*
 * If set to true, many of the scrambling transformations (e.g., shuffling of
 * assertions, permutation of names, etc.) will not be applied.
 */
bool no_scramble = false;

/*
 * If *not* set to true, the following modifications will be made additionally:
 * 1. The command (set-option :print-success false) is prepended.
 */
bool gen_incremental = false;

/*
 * If set to true, the following modifications will be made additionally:
 * 1. The command (set-option :produce-unsat-cores true) will be prepended.
 * 2. A (get-unsat-core) command will be inserted after each (check-sat) command.
 * 3. Each (assert fmla) will be replaced by (assert (! fmla :named freshId))
 *    where freshId is some fresh identifier.
 */
bool gen_ucore = false;

/*
 * If set to true, the following modifications will be made additionally:
 * 1. The command (set-option :produce-models true) will be prepended.
 * 2. A (get-model) command will be inserted after each (check-sat) command.
 */
bool gen_mval = false;

/*
 * If set to true, the following modifications will be made additionally:
 * 1. The command (set-option :produce-proofs true) will be prepended.
 * 2. A (get-proof) command will be inserted after each (check-sat) command.
 */
bool gen_proof = false;

/*
 * If set to true, support SMTLIB files that have features not supported by
 * SMTCOMP
 */
bool support_non_smtcomp = false;

/*
 * If set to true, support SMTLIB files that have Z3-specific features
 */
bool support_z3 = false;

/*
 * If set to true, the system prints the number of assertions to stdout
 */
bool count_asrts = false;

/*
    stores the name of the query intended to be fed into the 
*/
std::string ranks_file_name;

////////////////////////////////////////////////////////////////////////////////

/*
 * Scrambling of symbols (i.e., names) declared in the benchmark. For
 * details see "Scrambling and Descrambling SMT-LIB Benchmarks" (Tjark
 * Weber; in Tim King and Ruzica Piskac, editors, Proceedings of the
 * 14th International Workshop on Satisfiability Modulo Theories,
 * Coimbra, Portugal, July 1-2, 2016, volume 1617 of CEUR Workshop
 * Proceedings, pages 31-40, July 2016).
 *
 * There are three kinds of names: (1) names declared in the input
 * benchmark (e.g., sort symbols, function symbols, bound variables);
 * (2) name identifiers used during parsing; and (3) uniform names
 * (i.e., x1, x2, ...) used when the scrambled benchmark is printed.
 *
 * Benchmark-declared names are read during parsing and stored in the
 * nodes of the parse tree; specifically, in their symbol field (which
 * is otherwise also used to store SMT-LIB commands, keywords, etc.).
 *
 * In addition, a bijection between benchmark-declared names and name
 * identifiers is built during parsing, and extended whenever a
 * declaration or binder (of a new name) is encountered. Note that
 * name identifiers are not necessarily unique, i.e., they do not
 * resolve shadowing.
 *
 * Finally, when the scrambled benchmark is printed, name identifiers
 * are permuted randomly before they are turned into uniform names.
 */

typedef std::unordered_map<std::string, uint64_t> Name_ID_Map;

// a map from benchmark-declared symbols to name identifiers
Name_ID_Map name_ids;

// |foo| and foo denote the same symbol in SMT-LIB, hence the need to
// remove |...| quotes before symbol lookups
const char *unquote(const char *n)
{
    if (!n[0] || n[0] != '|') {
        return n;
    }

    static std::string buf;
    buf = n;
    assert(!buf.empty());
    if (buf.size() > 1 && buf[0] == '|' && buf[buf.size()-1] == '|') {
        buf = buf.substr(1, buf.size()-2);
    }
    return buf.c_str();
}

// the next available name id
uint64_t next_name_id = 1;

namespace scrambler {

// declaring a new name
void set_new_name(const char *n)
{
    n = unquote(n);

    if (name_ids.find(n) == name_ids.end()) {
        name_ids[n] = next_name_id;
        ++next_name_id;
    }
}

} // namespace

uint64_t get_name_id(const char *n)
{
    n = unquote(n);

    return name_ids[n];  // 0 if n is not currently in name_ids
}

////////////////////////////////////////////////////////////////////////////////

namespace scrambler {

void node::add_children(const std::vector<node *> *c)
{
    children.insert(children.end(), c->begin(), c->end());
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

/*
 * The main data structure: here the benchmark's commands are added as
 * they are parsed (and removed when they have been printed).
 */
std::vector<scrambler::node *> commands;

namespace scrambler {

void add_node(const char *s, node *n1, node *n2, node *n3, node *n4)
{
    assert(s); // s must be a top-level SMT-LIB command

    node *ret = new node;
    ret->symbol = s;
    ret->is_name = false;
    ret->needs_parens = true;

    if (n1) {
        ret->children.push_back(n1);
    }
    if (n2) {
        ret->children.push_back(n2);
    }
    if (n3) {
        ret->children.push_back(n3);
    }
    if (n4) {
        ret->children.push_back(n4);
    }

    commands.push_back(ret);
}

node *make_node(const char *s, node *n1, node *n2)
{
    node *ret = new node;
    ret->needs_parens = true;
    if (s) {
        ret->symbol = s;
    }
    ret->is_name = false;
    if (n1) {
        ret->children.push_back(n1);
    }
    if (n2) {
        ret->children.push_back(n2);
    }
    if (!ret->symbol.empty() && ret->children.empty()) {
        ret->needs_parens = false;
    }
    return ret;
}

node *make_node(const std::vector<node *> *v)
{
    node *ret = new node;
    ret->needs_parens = true;
    ret->symbol = "";
    ret->is_name = false;
    ret->children.assign(v->begin(), v->end());
    return ret;
}

node *make_node(node *n, const std::vector<node *> *v)
{
    node *ret = new node;
    ret->needs_parens = true;
    ret->symbol = "";
    ret->is_name = false;
    ret->children.push_back(n);
    ret->children.insert(ret->children.end(), v->begin(), v->end());
    return ret;
}

node *make_name_node(const char* s, node *n1)
{
    node *ret = new node;
    assert(s);
    ret->symbol = s;
    ret->is_name = true;
    ret->needs_parens = false;
    if (n1) {
        ret->children.push_back(n1);
        ret->needs_parens = true;
    }
    return ret;
}

void del_node(node *n)
{
    for (size_t i = 0; i < n->children.size(); ++i) {
        del_node(n->children[i]);
    }
    delete n;
}

} // scrambler

////////////////////////////////////////////////////////////////////////////////

namespace scrambler {

void shuffle_list(std::vector<scrambler::node *> *v, size_t start, size_t end)
{
    if (!no_scramble) {
        size_t n = end - start;
        for (size_t i = n-1; i > 0; --i) {
            std::swap((*v)[i+start], (*v)[next_rand_int(i+1)+start]);
        }
    }
}

void shuffle_list(std::vector<node *> *v)
{
    shuffle_list(v, 0, v->size());
}

} // scrambler

////////////////////////////////////////////////////////////////////////////////

/*
 * functions that set or depend on the benchmark's logic
 */

std::string logic;

namespace scrambler {

void set_logic(const std::string &l)
{
    // each benchmark contains a single set-logic command
    if (!logic.empty()) {
        std::cerr << "ERROR logic is already set" << std::endl;
        exit(1);
    }

    logic = l;
}

} // scrambler

bool logic_is_dl()  // Difference Logic: IDL, RDL
{
    static int result = -1;
    if (result == -1) {
        if (logic.empty()) {
            std::cerr << "ERROR logic has not been set" << std::endl;
            exit(1);
        }

        if (logic.find("IDL") != std::string::npos || logic.find("RDL") != std::string::npos) {
            result = 1;
        } else {
            result = 0;
        }
    }

    return (result == 1);
}

bool logic_is_arith()  // Arithmetic: IA, RA, IRA
{
    static int result = -1;
    if (result == -1) {
        if (logic.empty()) {
            std::cerr << "ERROR logic has not been set" << std::endl;
            exit(1);
        }

        if (logic.find("IA") != std::string::npos || logic.find("RA") != std::string::npos) {
            result = 1;
        } else {
            result = 0;
        }
    }

    return (result == 1);
}

bool logic_is_bv()  // BitVectors (BV)
{
    static int result = -1;
    if (result == -1) {
        if (logic.empty()) {
            std::cerr << "ERROR logic has not been set" << std::endl;
            exit(1);
        }

        if (logic.find("BV") != std::string::npos) {
            result = 1;
        } else {
            result = 0;
        }
    }

    return result == 1;
}

bool logic_is_fp()  // FloatingPoint (FP)
{
    static int result = -1;
    if (result == -1) {
        if (logic.empty()) {
            std::cerr << "ERROR logic has not been set" << std::endl;
            exit(1);
        }

        if (logic.find("FP") != std::string::npos) {
            result = 1;
        } else {
            result = 0;
        }
    }

    return result == 1;
}

namespace scrambler {

// Return vector index >= 0 (from where the list of children is commutative)
// if true, else -1.
int is_commutative(const node *n)
{
    // *n might be a qualified identifier of the form ('as' identifier sort)
    const std::string *symbol = &(n->symbol);
    if (*symbol == "as") {
        assert(n->children.size() > 0);
        symbol = &(n->children[0]->symbol);
    }
    const std::string &s = *symbol;
    assert(!s.empty());

    // Core theory
    if (s == "and" || s == "or" || s == "xor" || s == "distinct") {
        return 0;
    }
    if (!logic_is_dl()) {
        if (s == "=") {
            return 0;
        }
    }

    // arithmetic (IA, RA, IRA) (but not difference logic)
    if (logic_is_arith()) {
        if (s == "*" || s == "+") {
            return 0;
        }
    }

    // BitVectors
    if (logic_is_bv()) {
        if (s == "bvand" || s == "bvor" || s == "bvxor" ||
            s == "bvnand" || s == "bvnor" || s == "bvcomp" ||
            s == "bvadd" || s == "bvmul") {
            return 0;
        }
    }

    // FloatingPoint
    if (logic_is_fp()) {
        if (s == "fp.eq") {
            return 0;
        }
        if (s == "fp.add" || s == "fp.mul") {
            return 1;
        }
    }

    return -1;
}

bool flip_antisymm(const node *n, node ** const out_n)
{
    if (no_scramble) {
        return false;
    }

    if (!next_rand_int(2)) {
        return false;
    }

    // *n might be a qualified identifier of the form ('as' identifier sort)
    const std::string *symbol = &(n->symbol);
    if (*symbol == "as") {
        assert(n->children.size() > 0);
        symbol = &(n->children[0]->symbol);
    }
    const std::string &s = *symbol;
    assert(!s.empty());

    // arithmetic (IA, RA, IRA) (but not difference logic)
    if (logic_is_arith()) {
        if (s == "<") {
            *out_n = make_node(">");
            return true;
        } else if (s == ">") {
            *out_n = make_node("<");
            return true;
        } else if (s == "<=") {
            *out_n = make_node(">=");
            return true;
        } else if (s == ">=") {
            *out_n = make_node("<=");
            return true;
        }
    }

    // BitVectors
    if (logic_is_bv()) {
        if (s == "bvslt") {
            *out_n = make_node("bvsgt");
            return true;
        } else if (s == "bvsle") {
            *out_n = make_node("bvsge");
            return true;
        } else if (s == "bvult") {
            *out_n = make_node("bvugt");
            return true;
        } else if (s == "bvule") {
            *out_n = make_node("bvuge");
            return true;
        } else if (s == "bvsgt") {
            *out_n = make_node("bvslt");
            return true;
        } else if (s == "bvsge") {
            *out_n = make_node("bvsle");
            return true;
        } else if (s == "bvugt") {
            *out_n = make_node("bvult");
            return true;
        } else if (s == "bvuge") {
            *out_n = make_node("bvule");
            return true;
        }
    }

    // FloatingPoint
    if (logic_is_fp()) {
        if (s == "fp.leq") {
            *out_n = make_node("fp.geq");
            return true;
        } else if (s == "fp.lt") {
            *out_n = make_node("fp.gt");
            return true;
        } else if (s == "fp.geq") {
            *out_n = make_node("fp.leq");
            return true;
        } else if (s == "fp.gt") {
            *out_n = make_node("fp.lt");
            return true;
        }
    }

    return false;
}

} // scrambler

////////////////////////////////////////////////////////////////////////////////

/*
 * (scrambled) printing of benchmarks
 */

// a random permutation of name ids
std::vector<uint64_t> permuted_name_ids;

// uniform names
std::string make_name(uint64_t name_id)
{
    std::ostringstream tmp;
    tmp << "x" << name_id;
    return tmp.str();
}

// annotated assertions (for -gen-unsat-core true)
std::string make_annotation_name()
{
    static uint64_t n = 1;
    std::ostringstream tmp;
    tmp << "smtcomp" << n;
    ++n;
    return tmp.str();
}

static bool keep_annotation(const scrambler::node *n, annotation_mode keep_annotations) {
    if (keep_annotations == none)
        return false;
    if (keep_annotations == all)
        return true;
    assert(keep_annotations == pattern);
    return n->children.size() == 2 && n->children[1]->symbol == ":pattern";
}

void print_node(std::ostream &out, const scrambler::node *n, annotation_mode keep_annotations)
{
    if (n->symbol == "!" && !keep_annotation(n, keep_annotations)) {
        print_node(out, n->children[0], keep_annotations);
    } else {
        if (n->needs_parens) {
            out << '(';
        }
        if (!n->symbol.empty()) {
            if (no_scramble || !n->is_name) {
                out << n->symbol;
            } else {
                uint64_t name_id = get_name_id(n->symbol.c_str());
                if (name_id == 0) {
                    out << n->symbol;
                } else {
                    assert(name_id < permuted_name_ids.size());
                    out << make_name(permuted_name_ids[name_id]);
                }
            }
        }
        std::string name;
        if (gen_ucore && n->symbol == "assert") {
            name = make_annotation_name();
        }
        if (!name.empty()) {
            out << " (!";
        }
        for (size_t i = 0; i < n->children.size(); ++i) {
            if (i > 0 || !n->symbol.empty()) {
                out << ' ';
            }
            print_node(out, n->children[i], keep_annotations);
        }
        if (!name.empty()) {
            out << " :named " << name << ")";
        }
        if (n->needs_parens) {
            out << ')';
        }
        if (n->symbol == "check-sat") {
            if (gen_ucore) {
                // insert (get-unsat-core) after each check-sat
                out << std::endl << "(get-unsat-core)";
            }
            if (gen_mval) {
                // insert (get-model) after each check-sat
                out << std::endl << "(get-model)";
            }
            if (gen_proof) {
                // insert (get-proof) after each check-sat
                out << std::endl << "(get-proof)";
            }
        }
    }
}

void print_command(std::ostream &out, const scrambler::node *n, annotation_mode keep_annotations)
{
    print_node(out, n, keep_annotations);
    out << std::endl;
}

// ######################################################################################### //
// BEGIN FUNTIONS AND VARIABLES FOR RENAMING, DECLARATION SORTING, AND  SCRAMBLING VIA RANKS //
// ######################################################################################### //

// next available name id
uint64_t next_name_id_sorted = 1; 

// map of "names" (variables, functions, etc) to their corresponding name id
Name_ID_Map name_ids_sorted; 

// declaring a new name
void set_new_name_sorted(const char *n)
{
    n = unquote(n);

    if (name_ids_sorted.find(n) == name_ids_sorted.end()) {
        name_ids_sorted[n] = next_name_id_sorted;
        ++next_name_id_sorted;
    }
}

// getting a name's corresponding name id
uint64_t get_name_id_sorted(const char *n)
{
    n = unquote(n);

    return name_ids_sorted[n];  // 0 if n is not currently in name_ids
}

// used to find nodes where is_name is true and assigns them the next available name id 
void assign_num(const scrambler::node *n){
    for (size_t i = 0; i < n->children.size(); i++) {
        scrambler::node *new_n = n->children[i];
        if (!new_n->symbol.empty() && new_n -> is_name && new_n ->symbol != "=") {
            set_new_name_sorted(new_n -> symbol.c_str());
        }
    }
    for (size_t i = 0; i < n->children.size(); ++i) {
        if (i > 0 || !n->symbol.empty()) {
            assign_num(n->children[i]);
        }
    }
}

// returns the first occurence of a node where is_name is true, returns 0 if no such node is found
uint64_t find_var(const scrambler::node *n){
    for (size_t i = 0; i < n->children.size(); i++) {
        scrambler::node *new_n = n->children[i];
        if (!new_n->symbol.empty() && new_n -> is_name && new_n ->symbol != "=") {
            return get_name_id_sorted(new_n -> symbol.c_str());
        }
    }
    for (size_t i = 0; i < n->children.size(); ++i) {
        if (i > 0 || !n->symbol.empty()) {
            return find_var(n->children[i]);
        }
    }
    return 0;
}

// used to sort declarations and definitions based on a name id's first occurence
void sort_declarations(std::vector<scrambler::node *> *v, size_t start, size_t end){
    std::vector<std::pair<uint64_t, scrambler::node*>> combined_data;
    
    for (size_t i = start; i < end; ++i) {
        combined_data.push_back(std::make_pair(find_var((*v)[i]),(*v)[i]));
    }

    std::sort(combined_data.begin(), combined_data.end());
    
    for(size_t i = 0; i < end-start; i++){
        commands[i+start] = combined_data[i].second;
    }
}

// used to sort assertions based on a float vector of ranks
namespace scrambler{
    void shuffle_list(std::vector<scrambler::node *> *v, size_t start, size_t end, const std::vector<float> &ranks)
    {
        size_t n = end - start;
        std::vector<size_t> indices(n);
        for (size_t i = 0; i < n; ++i) indices[i] = i;
    
        std::sort(indices.begin(), indices.end(), [&ranks](size_t i, size_t j) { return ranks[i] < ranks[j]; });
    
        std::vector<scrambler::node *> temp(n);
        for (size_t i = 0; i < n; ++i)
            temp[i] = (*v)[start + indices[i]];
        for (size_t i = 0; i < n; ++i)
            (*v)[start + i] = temp[i];
    }
}

// used to test shuffle_list
std::vector<float> get_ranks(int size) {
    std::vector<float> output(size);
    std::ifstream file(ranks_file_name);
    if (!file.is_open()) {
        std::cerr << "Error opening ranks file: " << ranks_file_name << std::endl;
        return std::vector<float>(size, 0.0f); // return zeros
    }
    for (int i = 0; i < size; ++i) {
        if (!(file >> output[i])) {
            std::cerr << "Error reading ranks from file." << std::endl;
            return std::vector<float>(size, 0.0f); // return zeros if size is incorrect
        }
    }
    file.close();
    return output;
}

// modified version of print_node
void print_node_sorted(std::ostream &out, const scrambler::node *n, annotation_mode keep_annotations)
{
    if (n->symbol == "!" && !keep_annotation(n, keep_annotations)) {
        print_node_sorted(out, n->children[0], keep_annotations);
    } else {
        if (n->needs_parens) {
            out << '(';
        }
        if (!n->symbol.empty()) {
            if (no_scramble || !n->is_name) {
                out << n->symbol;
            } else {
                // uses get_name_id_sorted
                uint64_t name_id = get_name_id_sorted(n->symbol.c_str());
                if (name_id == 0) {
                    out << n->symbol;
                } else {
                    out << make_name(name_id);
                }
            }
        }
        std::string name;
        if (gen_ucore && n->symbol == "assert") {
            name = make_annotation_name();
        }
        if (!name.empty()) {
            out << " (!";
        }
        for (size_t i = 0; i < n->children.size(); ++i) {
            if (i > 0 || !n->symbol.empty()) {
                out << ' ';
            }
            print_node_sorted(out, n->children[i], keep_annotations);
        }
        if (!name.empty()) {
            out << " :named " << name << ")";
        }
        if (n->needs_parens) {
            out << ')';
        }
        if (n->symbol == "check-sat") {
            if (gen_ucore) {
                // insert (get-unsat-core) after each check-sat
                out << std::endl << "(get-unsat-core)";
            }
            if (gen_mval) {
                // insert (get-model) after each check-sat
                out << std::endl << "(get-model)";
            }
            if (gen_proof) {
                // insert (get-proof) after each check-sat
                out << std::endl << "(get-proof)";
            }
        }
    }
}

// modified version of print_command
void print_command_sorted(std::ostream &out, const scrambler::node *n, annotation_mode keep_annotations)
{
    // uses print_node_sorted
    print_node_sorted(out, n, keep_annotations);
    out << std::endl;
}


// modified version of print_scrambled
void print_ranked(std::ostream &out, annotation_mode keep_annotations)
{   
    // either run function to get scores or maybe feed it into this function? idk
    std::vector<float> ranks;

    // identify consecutive assertions and sort them
    // currently this breaks if assertions are in multiple discrete groups because it's lazily copied from print_scrambled
    for (size_t i = 0; i < commands.size(); ) {
        bool already = false;
        if (commands[i]->symbol == "assert" && !already) {
            already = true;
            size_t j = i+1;
            while (j < commands.size() && commands[j]->symbol == "assert"){ ++j; }
            ranks = get_ranks(j - i);
            if (j - i > 1) {
                shuffle_list(&commands, i, j, ranks);
            }
            i = j;
        } 
        else if (commands[i]->symbol == "assert" && already) {
            throw std::invalid_argument("assertions in multiple chunks");
        }
        else {
            ++i;
        }
    }

    // assign each variable a number in correspondence to
    // its first appearance in the newly sorted assertions
    for (size_t i = 0; i < commands.size(); i++){
        if (commands[i] -> symbol == "assert"){
            assign_num(commands[i]);
        }
    }

    // sort declarations and definitions    
    // currently this breaks if declarations or definitions are in multiple discrete groups because it's lazily copied from print_scrambled
    for (size_t i = 0; i < commands.size(); ) {
        bool already = false;
        if (((commands[i] -> symbol).find("declare") != std::string::npos ||(commands[i] -> symbol).find("define") != std::string::npos) && !already) {
            already = true;
            size_t j = i+1;
            while (j < commands.size() && ((commands[j] -> symbol).find("declare") != std::string::npos ||(commands[j] -> symbol).find("define") != std::string::npos)){ ++j; }
            if (j - i > 1) {
                sort_declarations(&commands, i, j);
            }
            i = j;
        } 
        else if (((commands[i] -> symbol).find("declare") != std::string::npos ||(commands[i] -> symbol).find("define") != std::string::npos) && already) {
            throw std::invalid_argument("declarations and definitions in multiple chunks");
        }
        else {
            ++i;
        }
    }

    // print all commands using print_commands_sorted
    for (size_t i = 0; i < commands.size(); ++i) {
        print_command_sorted(out, commands[i], keep_annotations);
        del_node(commands[i]);
    }
    commands.clear();
}

// ####################################################################################### //
// END FUNTIONS AND VARIABLES FOR RENAMING, DECLARATION SORTING, AND  SCRAMBLING VIA RANKS //
// ####################################################################################### //



void print_scrambled(std::ostream &out, annotation_mode keep_annotations)
{
    if (!no_scramble) {
        // identify consecutive declarations and shuffle them
        for (size_t i = 0; i < commands.size(); ) {
            if (commands[i]->symbol == "declare-fun") {
                size_t j = i+1;
                while (j < commands.size() &&
                       commands[j]->symbol == "declare-fun") {
                    ++j;
                }
                if (j - i > 1) {
                    shuffle_list(&commands, i, j);
                }
                i = j;
            } else {
                ++i;
            }
        }

        // identify consecutive assertions and shuffle them
        for (size_t i = 0; i < commands.size(); ) {
            if (commands[i]->symbol == "assert") {
                size_t j = i+1;
                while (j < commands.size() &&
                       commands[j]->symbol == "assert"){
                    ++j;
                }
                if (j - i > 1) {
                    shuffle_list(&commands, i, j);
                }
                i = j;
            } else {
                ++i;
            }
        }

        // Generate a random permutation of name ids. Note that index
        // 0 is unused in the permuted_name_ids vector (but present to
        // simplify indexing), and index next_name_id is out of range.
        size_t old_size = permuted_name_ids.size();
        assert(old_size <= next_name_id);
        // Since the print_scrambled function may be called multiple
        // times (for different parts of the benchmark), we only need
        // to permute those name ids that have been declared since the
        // last call to print_scrambled.
        if (old_size < next_name_id) {
            permuted_name_ids.reserve(next_name_id);
            for (size_t i = old_size; i < next_name_id; ++i) {
                permuted_name_ids.push_back(i);
                assert(permuted_name_ids[i] == i);
            }
            assert(permuted_name_ids.size() == next_name_id);
            // index 0 must not be shuffled
            if (old_size == 0) {
                old_size = 1;
            }
            // Knuth shuffle
            for (size_t i = old_size; i < next_name_id - 1; ++i) {
                size_t j = i + next_rand_int(next_name_id - i);
                std::swap(permuted_name_ids[i], permuted_name_ids[j]);
            }
        }
    }

    // print all commands
    for (size_t i = 0; i < commands.size(); ++i) {
        print_command(out, commands[i], keep_annotations);
        del_node(commands[i]);
    }
    commands.clear();
}

////////////////////////////////////////////////////////////////////////////////

/*
 * -core
 */

typedef std::unordered_set<std::string> StringSet;

bool parse_core(std::istream &src, StringSet &out)
{
    std::string name;
    src >> name;
    if (!src || name != "unsat") {
        return false;
    }
    // skip chars until a '(' is found
    char c;
    while (src.get(c) && c != '(') {
        if (!isspace(c)) {
            return false;
        }
    }
    if (!src) {
        return false;
    }
    bool done = false;
    while (src && !done) {
        src >> name;
        if (name.empty()) {
            return false;
        }
        if (name[name.size()-1] == ')') {
            name = name.substr(0, name.size()-1);
            done = true;
        }
        if (!name.empty()) {
            out.insert(name);
        }
    }

    std::vector<std::string> outnames(out.begin(), out.end());
    std::sort(outnames.begin(), outnames.end());

    std::cout << ";; parsed " << outnames.size() << " names:";
    for (size_t i = 0; i < outnames.size(); ++i) {
        std::cout << " " << outnames[i];
    }
    std::cout << std::endl;

    return true;
}

std::string get_named_annot(scrambler::node *root)
{
    std::vector<scrambler::node *> to_process;
    std::unordered_set<scrambler::node *> seen;

    to_process.push_back(root);
    while (!to_process.empty()) {
        scrambler::node *cur = to_process.back();
        to_process.pop_back();

        if (!seen.insert(cur).second) {
            continue;
        }
        if (cur->symbol == "!") {
            if (cur->children.size() >= 1) {
                to_process.push_back(cur->children[0]);
            }
            if (cur->children.size() >= 2) {
                for (size_t j = 1; j < cur->children.size(); ++j) {
                    scrambler::node *attr = cur->children[j];
                    if (attr->symbol == ":named" &&
                        !attr->children.empty()) {
                        return attr->children[0]->symbol;
                    }
                }
            }
        } else {
            for (size_t j = 0; j < cur->children.size(); ++j) {
                to_process.push_back(cur->children[j]);
            }
        }
    }

    return "";
}

// Used by the post-processor in the unsat core track to filter the
// assertions and only keep those that appear in the unsat core.
// The string set `to_keep` lists all names that should be kept.
void filter_named(const StringSet &to_keep)
{
    size_t i, k;
    for (i = k = 0; i < commands.size(); ++i) {
        scrambler::node *cur = commands[i];
        bool keep = true;
        if (cur->symbol == "assert") {
            std::string name = get_named_annot(cur);
            if (!name.empty() && to_keep.find(name) == to_keep.end()) {
                keep = false;
            }
        }
        if (keep) {
            commands[k++] = cur;
        }
    }
    commands.resize(k);
}

////////////////////////////////////////////////////////////////////////////////

char *c_strdup(const char *s)
{
    char *ret = (char *)malloc(strlen(s) + 1);
    if (ret == NULL) {
        exit(1);
    }

    strcpy(ret, s);
    return ret;
}

////////////////////////////////////////////////////////////////////////////////

void usage(const char *program)
{
    std::cout << "Syntax: " << program << " [OPTIONS] < INPUT_FILE.smt2\n"
              << "\n"
              << "    -term_annot [true|pattern|false]\n"
              << "        controls whether term annotations are printed "
                 "(default: true)\n"
              << "\n"
              << "    -seed N\n"
              << "        seed value (>= 0) for pseudo-random choices; if 0, "
                 "no scrambling is\n"
              << "        performed (default: time(0))\n"
              << "\n"
              << "    -core FILE\n"
              << "        print only those (named) assertions whose name is "
                 "contained in the\n"
              << "        specified FILE (default: print all assertions)\n"
              << "\n"
              << "    -incremental [true|false]\n"
              << "        produce output in a format suitable for the trace "
                 "executer used in\n"
              << "        the incremental track of SMT-COMP (default: false)\n"
              << "\n"
              << "    -gen-unsat-core [true|false]\n"
              << "        controls whether the output is in a format suitable "
                 "for the unsat-core\n"
              << "        track of SMT-COMP (default: false)\n"
              << "\n"
              << "    -gen-model-val [true|false]\n"
              << "        controls whether the output is in a format suitable "
                 "for the model\n"
              << "        validation track of SMT-COMP (default: false)\n"
              << "\n"
              << "    -gen-proof [true|false]\n"
              << "        controls whether the output is in a format suitable "
                 "for the proof\n"
              << "        track of SMT-COMP (default: false)\n"
              << "\n"
              << "    -support-non-smtcomp [true|false]\n"
              << "        controls whether to support SMTLIB commands that are "
                 "not supported\n"
              << "        by SMTCOMP (default: false)\n"
              << "\n"
              << "    -support-z3 [true|false]\n"
              << "        controls whether to support non-SMTLIB commands that "
                 "are supported\n"
              << "        by Z3 (default: false)\n"
              << "\n"
              << "    -count-asserts [true|false]\n"
              << "        controls whether the number of assertions found in the benchmark\n"
              << "        is printed to stderr (default: false)\n\n"
              << "    -ranks <file>\n"
              << "        specifies a file containing the ranks to be used for sorting\n\n";
    std::cout.flush();
    exit(1);
}

////////////////////////////////////////////////////////////////////////////////

extern int yyparse();

using namespace scrambler;

int main(int argc, char **argv)
{
    annotation_mode keep_annotations = all;

    bool create_core = false;
    std::string core_file;

    set_seed(time(0));

    for (int i = 1; i < argc; ) {
        if (strcmp(argv[i], "-seed") == 0 && i+1 < argc) {
            std::istringstream s(argv[i+1]);
            int x;
            if (s >> x && x >= 0) {
                if (x > 0) {
                    set_seed(x);
                } else {
                    no_scramble = true;
                }
            } else {
                std::cerr << "Invalid value for -seed: " << argv[i+1] << std::endl;
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-term_annot") == 0 && i+1 < argc) {
            if (strcmp(argv[i+1], "true") == 0) {
                keep_annotations = all;
            } else if (strcmp(argv[i+1], "pattern") == 0) {
                keep_annotations = pattern;
            } else if (strcmp(argv[i+1], "false") == 0) {
                keep_annotations = none;
            } else {
                std::cerr << "1" << std::endl;
                usage(argv[0]);
            }
            i += 2;
        } else if (strcmp(argv[i], "-core") == 0 && i+1 < argc) {
            create_core = true;
            core_file = argv[i+1];
            i += 2;
        } else if (strcmp(argv[i], "-incremental") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "true") == 0) {
                gen_incremental = true;
            } else if (strcmp(argv[i + 1], "false") == 0) {
                gen_incremental = false;
            } else {
                std::cerr << "2" << std::endl;
                usage(argv[0]);
            }
            i += 2;
        } else if (strcmp(argv[i], "-gen-unsat-core") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "true") == 0) {
                gen_ucore = true;
            } else if (strcmp(argv[i + 1], "false") == 0) {
                gen_ucore = false;
            } else {
                std::cerr << "3" << std::endl;
                usage(argv[0]);
            }
            i += 2;
        } else if (strcmp(argv[i], "-gen-model-val") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "true") == 0) {
                gen_mval = true;
            } else if (strcmp(argv[i + 1], "false") == 0) {
                gen_mval = false;
            } else {
                std::cerr << "4" << std::endl;
                usage(argv[0]);
            }
            i += 2;
        } else if (strcmp(argv[i], "-gen-proof") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "true") == 0) {
                gen_proof = true;
            } else if (strcmp(argv[i + 1], "false") == 0) {
                gen_proof = false;
            } else {
                std::cerr << "5" << std::endl;
                usage(argv[0]);
            }
            i += 2;
        } else if (strcmp(argv[i], "-support-non-smtcomp") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "true") == 0) {
                support_non_smtcomp = true;
            } else if (strcmp(argv[i + 1], "false") == 0) {
                support_non_smtcomp = false;
            } else {
                std::cerr << "6" << std::endl;
                usage(argv[0]);
            }
            i += 2;
        } else if (strcmp(argv[i], "-support-z3") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "true") == 0) {
                support_z3 = true;
            } else if (strcmp(argv[i + 1], "false") == 0) {
                support_z3 = false;
            } else {
                std::cerr << "7" << std::endl;
                usage(argv[0]);
            }
            i += 2;
        } else if (strcmp(argv[i], "-count-asserts") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "true") == 0) {
                count_asrts = true;
            } else if (strcmp(argv[i + 1], "false") == 0) {
                count_asrts = false;
            } else {
                std::cerr << "8" << std::endl;
                usage(argv[0]);
            }
            i += 2;
        } else if (strcmp(argv[i], "-ranks") == 0 && i + 1 < argc) {
            ranks_file_name = argv[i+1];
            std::cerr << "Ranks file: " << ranks_file_name << std::endl;
            i += 2;
        } else {
            std::cerr << "9" << std::endl;
            std::cerr << "argc: " << argc << std::endl;
            std::cerr << "i: " << i << std::endl;
            usage(argv[0]);
        }
    }

    StringSet core_names;
    if (create_core) {
        std::ifstream src(core_file.c_str());
        if (!parse_core(src, core_names)) {
            std::cerr << "ERROR parsing core names from " << core_file << std::endl;
            return 1;
        }
    }

    if (!gen_incremental && !count_asrts) {
        // prepend SMT-LIB command that suppresses success for non-incremental
        // tracks
        std::cout << "(set-option :print-success false)" << std::endl;
    }

    if (gen_ucore) {
        // prepend SMT-LIB command that enables production of unsat cores
        std::cout << "(set-option :produce-unsat-cores true)" << std::endl;
    }

    if (gen_mval) {
        // prepend SMT-LIB command that enables production of models
        std::cout << "(set-option :produce-models true)" << std::endl;
    }

    if (gen_proof) {
        // prepend SMT-LIB command that enables production of models
        std::cout << "(set-option :produce-proofs true)" << std::endl;
    }

    if (count_asrts) {
        while (!std::cin.eof()) {
            yyparse();
        }
        int asrt_count = 0;
        for (std::vector<scrambler::node*>::iterator it = commands.begin(); it != commands.end(); ++it) {
            if ((*it)->symbol == "assert") {
                asrt_count++;
            }
        }
        std::cerr << "; Number of assertions: " << asrt_count << "\n";
        exit(0);
    }

    while (!std::cin.eof()) {
        yyparse();
        if (!commands.empty() && commands.back()->symbol == "check-sat") {
            if (create_core) {
                filter_named(core_names);
            }
            assert(!commands.empty());
            // print_scrambled(std::cout, keep_annotations);
            print_ranked(std::cout, keep_annotations);
        }
    }

    if (create_core) {
        filter_named(core_names);
    }
    if (!commands.empty()) {
        // print_scrambled(std::cout, keep_annotations);
        print_ranked(std::cout, keep_annotations);

    }

    return 0;
}
