
#include "CFG.h"
#include <algorithm>
#include <algorithm> // sort
#include <climits>   // INT_MAX
#include <sstream>
#include <unordered_map>
#include <unordered_set>

// helps debugging
template class std::unique_ptr<ruby_typer::cfg::CFG>;
template class std::unique_ptr<ruby_typer::cfg::Instruction>;

using namespace std;

namespace ruby_typer {
namespace cfg {

int CFG::FORWARD_TOPO_SORT_VISITED = 1 << 0;
int CFG::BACKWARD_TOPO_SORT_VISITED = 1 << 1;

core::SymbolRef maybeDealias(core::Context ctx, core::SymbolRef what,
                             unordered_map<core::SymbolRef, core::SymbolRef> &aliases) {
    if (what.info(ctx).isSyntheticTemporary(ctx)) {
        auto fnd = aliases.find(what);
        if (fnd != aliases.end()) {
            return fnd->second;
        } else
            return what;
    } else {
        return what;
    }
}

/**
 * Remove aliases from CFG. Why does this need a separate pass?
 * because `a.foo(a = "2", if (...) a = true; else a = null; end)`
 */
void CFG::dealias(core::Context ctx) {
    vector<unordered_map<core::SymbolRef, core::SymbolRef>> outAliases;

    outAliases.resize(this->basicBlocks.size());
    for (BasicBlock *bb : this->backwardsTopoSort) {
        if (bb == this->deadBlock())
            continue;
        unordered_map<core::SymbolRef, core::SymbolRef> &current = outAliases[bb->id];
        if (bb->backEdges.size() > 0) {
            current = outAliases[bb->backEdges[0]->id];
        }

        for (BasicBlock *parent : bb->backEdges) {
            unordered_map<core::SymbolRef, core::SymbolRef> other = outAliases[parent->id];
            for (auto it = current.begin(); it != current.end(); /* nothing */) {
                auto &el = *it;
                auto fnd = other.find(el.first);
                if (fnd != other.end()) {
                    if (fnd->second != el.second) {
                        it = current.erase(it);
                    } else {
                        ++it;
                    }
                } else {
                    ++it;
                }
            }
        }

        for (Binding &bind : bb->exprs) {
            if (auto *i = dynamic_cast<Ident *>(bind.value.get())) {
                i->what = maybeDealias(ctx, i->what, current);
            }
            /* invalidate a stale record */
            for (auto it = current.begin(); it != current.end(); /* nothing */) {
                if (it->second == bind.bind) {
                    it = current.erase(it);
                } else {
                    ++it;
                }
            }
            /* dealias */
            if (auto *v = dynamic_cast<Ident *>(bind.value.get())) {
                v->what = maybeDealias(ctx, v->what, current);
            } else if (auto *v = dynamic_cast<Send *>(bind.value.get())) {
                v->recv = maybeDealias(ctx, v->recv, current);
                for (auto &arg : v->args) {
                    arg = maybeDealias(ctx, arg, current);
                }
            } else if (auto *v = dynamic_cast<Super *>(bind.value.get())) {
                for (auto &arg : v->args) {
                    arg = maybeDealias(ctx, arg, current);
                }
            } else if (auto *v = dynamic_cast<Return *>(bind.value.get())) {
                v->what = maybeDealias(ctx, v->what, current);
            } else if (auto *v = dynamic_cast<NamedArg *>(bind.value.get())) {
                v->value = maybeDealias(ctx, v->value, current);
            }

            // record new aliases
            if (auto *i = dynamic_cast<Ident *>(bind.value.get())) {
                current[bind.bind] = i->what;
            }
        }
    }
}

void CFG::fillInBlockArguments(core::Context ctx) {
    // Dmitry's algorithm for adding basic block arguments
    // I don't remember this version being described in any book.
    //
    // Compute two upper bounds:
    //  - one by accumulating all reads on the reverse graph
    //  - one by accumulating all writes on direct graph
    //
    //  every node gets the intersection between two sets suggested by those overestimations.
    //
    // This solution is  (|BB| + |symbols-mentioned|) * (|cycles|) + |answer_size| in complexity.
    // making this quadratic in anything will be bad.
    unordered_map<core::SymbolRef, unordered_set<BasicBlock *>> reads;
    unordered_map<core::SymbolRef, unordered_set<BasicBlock *>> writes;

    for (unique_ptr<BasicBlock> &bb : this->basicBlocks) {
        for (Binding &bind : bb->exprs) {
            writes[bind.bind].insert(bb.get());
            if (auto *v = dynamic_cast<Ident *>(bind.value.get())) {
                reads[v->what].insert(bb.get());
            } else if (auto *v = dynamic_cast<Send *>(bind.value.get())) {
                reads[v->recv].insert(bb.get());
                for (auto arg : v->args) {
                    reads[arg].insert(bb.get());
                }
            } else if (auto *v = dynamic_cast<Super *>(bind.value.get())) {
                for (auto arg : v->args) {
                    reads[arg].insert(bb.get());
                }
            } else if (auto *v = dynamic_cast<Return *>(bind.value.get())) {
                reads[v->what].insert(bb.get());
            } else if (auto *v = dynamic_cast<NamedArg *>(bind.value.get())) {
                reads[v->value].insert(bb.get());
            } else if (auto *v = dynamic_cast<LoadArg *>(bind.value.get())) {
                reads[v->receiver].insert(bb.get());
            }
        }
        if (bb->bexit.cond != ctx.state.defn_cfg_never() && bb->bexit.cond != ctx.state.defn_cfg_always()) {
            reads[bb->bexit.cond].insert(bb.get());
        }
    }

    for (auto &pair : reads) {
        core::SymbolRef what = pair.first;
        if (!what.info(ctx).isLocalVariable())
            continue;
        unordered_set<BasicBlock *> &where = pair.second;
        int &min = what.info(ctx).minLoops;

        for (BasicBlock *bb : where) {
            if (min > bb->outerLoops) {
                min = bb->outerLoops;
            }
        }
    }

    for (auto &pair : writes) {
        core::SymbolRef what = pair.first;
        if (!what.info(ctx).isLocalVariable())
            continue;
        unordered_set<BasicBlock *> &where = pair.second;
        int &min = what.info(ctx).minLoops;

        for (BasicBlock *bb : where) {
            if (min > bb->outerLoops) {
                min = bb->outerLoops;
            }
        }
    }

    for (auto &it : this->basicBlocks) {
        /* remove dead variables */
        for (auto expIt = it->exprs.begin(); expIt != it->exprs.end(); /* nothing */) {
            Binding &bind = *expIt;
            auto fnd = reads.find(bind.bind);
            if (fnd == reads.end()) {
                // This should be !New && !Send && !Return, but I prefer to list explicitly in case we start adding
                // nodes.
                if (dynamic_cast<Ident *>(bind.value.get()) != nullptr ||
                    dynamic_cast<ArraySplat *>(bind.value.get()) != nullptr ||
                    dynamic_cast<HashSplat *>(bind.value.get()) != nullptr ||
                    dynamic_cast<BoolLit *>(bind.value.get()) != nullptr ||
                    dynamic_cast<StringLit *>(bind.value.get()) != nullptr ||
                    dynamic_cast<IntLit *>(bind.value.get()) != nullptr ||
                    dynamic_cast<FloatLit *>(bind.value.get()) != nullptr ||
                    dynamic_cast<Self *>(bind.value.get()) != nullptr ||
                    dynamic_cast<LoadArg *>(bind.value.get()) != nullptr ||
                    dynamic_cast<NamedArg *>(bind.value.get()) != nullptr) {
                    expIt = it->exprs.erase(expIt);
                } else {
                    ++expIt;
                }
            } else {
                ++expIt;
            }
        }
    }

    vector<unordered_set<core::SymbolRef>> reads_by_block(this->basicBlocks.size());
    vector<unordered_set<core::SymbolRef>> writes_by_block(this->basicBlocks.size());

    for (auto &rds : reads) {
        auto &wts = writes[rds.first];
        if (rds.second.size() == 1 && wts.size() == 1 && *(rds.second.begin()) == *(wts.begin())) {
            wts.clear();
            rds.second.clear(); // remove symref that never escapes a block.
        } else if (wts.empty()) {
            rds.second.clear();
        }
    }

    for (auto &wts : writes) {
        auto &rds = reads[wts.first];
        if (rds.empty()) {
            wts.second.clear();
        }
        for (BasicBlock *bb : rds) {
            reads_by_block[bb->id].insert(wts.first);
        }
        for (BasicBlock *bb : wts.second) {
            writes_by_block[bb->id].insert(wts.first);
        }
    }

    // iterate ver basic blocks in reverse and found upper bounds on what could a block need.
    vector<unordered_set<core::SymbolRef>> upper_bounds1(this->basicBlocks.size());
    bool changed = true;

    while (changed) {
        changed = false;
        for (BasicBlock *bb : this->forwardsTopoSort) {
            int sz = upper_bounds1[bb->id].size();
            upper_bounds1[bb->id].insert(reads_by_block[bb->id].begin(), reads_by_block[bb->id].end());
            if (bb->bexit.thenb != deadBlock()) {
                upper_bounds1[bb->id].insert(upper_bounds1[bb->bexit.thenb->id].begin(),
                                             upper_bounds1[bb->bexit.thenb->id].end());
            }
            if (bb->bexit.elseb != deadBlock()) {
                upper_bounds1[bb->id].insert(upper_bounds1[bb->bexit.elseb->id].begin(),
                                             upper_bounds1[bb->bexit.elseb->id].end());
            }
            changed = changed || (upper_bounds1[bb->id].size() != sz);
        }
    }

    vector<unordered_set<core::SymbolRef>> upper_bounds2(this->basicBlocks.size());

    changed = true;
    while (changed) {
        changed = false;
        for (auto it = this->backwardsTopoSort.begin(); it != this->backwardsTopoSort.end(); ++it) {
            BasicBlock *bb = *it;
            int sz = upper_bounds2[bb->id].size();
            upper_bounds2[bb->id].insert(writes_by_block[bb->id].begin(), writes_by_block[bb->id].end());
            for (BasicBlock *edge : bb->backEdges) {
                if (edge != deadBlock()) {
                    upper_bounds2[bb->id].insert(upper_bounds2[edge->id].begin(), upper_bounds2[edge->id].end());
                }
            }
            changed = changed || (upper_bounds2[bb->id].size() != sz);
        }
    }

    /** Combine two upper bounds */
    for (auto &it : this->basicBlocks) {
        auto set2 = upper_bounds2[it->id];

        int set1Sz = set2.size();
        int set2Sz = upper_bounds1[it->id].size();
        it->args.reserve(set1Sz > set2Sz ? set2Sz : set1Sz);
        for (auto el : upper_bounds1[it->id]) {
            if (set2.find(el) != set2.end()) {
                it->args.push_back(el);
            }
        }
        sort(it->args.begin(), it->args.end(),
             [](core::SymbolRef a, core::SymbolRef b) -> bool { return a._id < b._id; });
    }

    return;
}

int CFG::topoSortFwd(vector<BasicBlock *> &target, int nextFree, BasicBlock *currentBB) {
    // Error::check(!marked[currentBB]) // graph is cyclic!
    if ((currentBB->flags & FORWARD_TOPO_SORT_VISITED)) {
        return nextFree;
    } else {
        currentBB->flags |= FORWARD_TOPO_SORT_VISITED;
        nextFree = topoSortFwd(target, nextFree, currentBB->bexit.thenb);
        nextFree = topoSortFwd(target, nextFree, currentBB->bexit.elseb);
        target[nextFree] = currentBB;
        return nextFree + 1;
    }
}

int CFG::topoSortBwd(vector<BasicBlock *> &target, int nextFree, BasicBlock *currentBB) {
    // We're not looking for an arbitrary topo-sort.
    // First of all topo sort does not exist, as the graph has loops.
    // We are looking for a sort that has all outer loops dominating loop headers that dominate loop bodies.
    //
    // This method is a big cache invalidator and should be removed if we become slow
    // Instead we will build this sort the fly during construction of the CFG, but it will make it hard to add new nodes
    // much harder.

    if ((currentBB->flags & BACKWARD_TOPO_SORT_VISITED)) {
        return nextFree;
    } else {
        currentBB->flags |= BACKWARD_TOPO_SORT_VISITED;
        int i = 0;
        // iterate over outer loops
        while (i < currentBB->backEdges.size() && currentBB->outerLoops > currentBB->backEdges[i]->outerLoops) {
            nextFree = topoSortBwd(target, nextFree, currentBB->backEdges[i]);
            i++;
        }
        if (i > 0) { // This is a loop header!
            target[nextFree] = currentBB;
            nextFree = nextFree + 1;
            while (i < currentBB->backEdges.size()) {
                nextFree = topoSortBwd(target, nextFree, currentBB->backEdges[i]);
                i++;
            }
        } else {
            while (i < currentBB->backEdges.size()) {
                nextFree = topoSortBwd(target, nextFree, currentBB->backEdges[i]);
                i++;
            }
            target[nextFree] = currentBB;
            nextFree = nextFree + 1;
        }
        return nextFree;
    }
}

void CFG::fillInTopoSorts(core::Context ctx) {
    // needed to find loop headers.
    for (auto &bb : this->basicBlocks) {
        std::sort(bb->backEdges.begin(), bb->backEdges.end(),
                  [](const BasicBlock *a, const BasicBlock *b) -> bool { return a->outerLoops < b->outerLoops; });
    }

    auto &target1 = this->forwardsTopoSort;
    target1.resize(this->basicBlocks.size());
    this->topoSortFwd(target1, 0, this->entry());

    auto &target2 = this->backwardsTopoSort;
    target2.resize(this->basicBlocks.size());
    this->topoSortBwd(target2, 0, this->deadBlock());
    return;
}

void jumpToDead(BasicBlock *from, CFG &inWhat);

unique_ptr<CFG> CFG::buildFor(core::Context ctx, ast::MethodDef &md) {
    unique_ptr<CFG> res(new CFG); // private constructor
    res->symbol = md.symbol;
    core::SymbolRef retSym =
        ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::returnMethodTemp(), md.symbol);
    core::SymbolRef selfSym =
        ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::selfMethodTemp(), md.symbol);

    BasicBlock *entry = res->entry();

    entry->exprs.emplace_back(selfSym, md.loc, make_unique<Self>(md.symbol.info(ctx).owner));
    auto methodName = md.symbol.info(ctx).name;

    int i = 0;
    for (core::SymbolRef argSym : md.symbol.info(ctx).arguments()) {
        entry->exprs.emplace_back(argSym, argSym.info(ctx).definitionLoc, make_unique<LoadArg>(selfSym, methodName, i));
        i++;
    }
    std::unordered_map<core::SymbolRef, core::SymbolRef> aliases;
    auto cont = res->walk(ctx, md.rhs.get(), entry, *res.get(), retSym, 0, aliases);
    core::SymbolRef retSym1 =
        ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::returnMethodTemp(), md.symbol);

    cont->exprs.emplace_back(retSym1, md.loc, make_unique<Return>(retSym)); // dead assign.
    jumpToDead(cont, *res.get());

    std::vector<Binding> aliasesPrefix;
    for (auto kv : aliases) {
        core::SymbolRef global = kv.first;
        core::SymbolRef local = kv.second;
        local.info(ctx).minLoops = -1;
        aliasesPrefix.emplace_back(local, md.symbol.info(ctx).definitionLoc, make_unique<Alias>(global));
    }
    std::sort(aliasesPrefix.begin(), aliasesPrefix.end(),
              [](const Binding &l, const Binding &r) -> bool { return l.bind._id < r.bind._id; });

    entry->exprs.insert(entry->exprs.begin(), make_move_iterator(aliasesPrefix.begin()),
                        make_move_iterator(aliasesPrefix.end()));

    res->fillInTopoSorts(ctx);
    res->dealias(ctx);
    res->fillInBlockArguments(ctx);
    return res;
}

BasicBlock *CFG::freshBlock(int outerLoops) {
    int id = this->basicBlocks.size();
    this->basicBlocks.emplace_back(new BasicBlock());
    BasicBlock *r = this->basicBlocks.back().get();
    r->id = id;
    r->outerLoops = outerLoops;
    return r;
}

CFG::CFG() {
    freshBlock(0); // entry;
    freshBlock(0); // dead code;
    deadBlock()->bexit.elseb = deadBlock();
    deadBlock()->bexit.thenb = deadBlock();
    deadBlock()->bexit.cond = core::GlobalState::defn_cfg_never();
}

void conditionalJump(BasicBlock *from, core::SymbolRef cond, BasicBlock *thenb, BasicBlock *elseb, CFG &inWhat) {
    if (from != inWhat.deadBlock()) {
        Error::check(!from->bexit.cond.exists());
        from->bexit.cond = cond;
        from->bexit.thenb = thenb;
        from->bexit.elseb = elseb;
        thenb->backEdges.push_back(from);
        elseb->backEdges.push_back(from);
    }
}

void unconditionalJump(BasicBlock *from, BasicBlock *to, CFG &inWhat) {
    if (from != inWhat.deadBlock()) {
        Error::check(!from->bexit.cond.exists());
        from->bexit.cond = core::GlobalState::defn_cfg_always();
        from->bexit.elseb = to;
        from->bexit.thenb = to;
        to->backEdges.push_back(from);
    }
}

void jumpToDead(BasicBlock *from, CFG &inWhat) {
    auto *db = inWhat.deadBlock();
    if (from != db) {
        Error::check(!from->bexit.cond.exists());
        from->bexit.cond = core::GlobalState::defn_cfg_never();
        from->bexit.elseb = db;
        from->bexit.thenb = db;
        db->backEdges.push_back(from);
    }
}

core::SymbolRef global2Local(core::Context ctx, core::SymbolRef what, CFG &inWhat,
                             std::unordered_map<core::SymbolRef, core::SymbolRef> &aliases) {
    core::Symbol &info = what.info(ctx);
    if (!info.isLocalVariable()) {
        core::SymbolRef &alias = aliases[what];
        if (!alias.exists()) {
            alias = ctx.state.newTemporary(core::UniqueNameKind::CFG, info.name, inWhat.symbol);
        }
        return alias;
    } else {
        return what;
    }
}

/** Convert `what` into a cfg, by starting to evaluate it in `current` inside method defined by `inWhat`.
 * store result of evaluation into `target`. Returns basic block in which evaluation should proceed.
 */
BasicBlock *CFG::walk(core::Context ctx, ast::Expression *what, BasicBlock *current, CFG &inWhat,
                      core::SymbolRef target, int loops,
                      std::unordered_map<core::SymbolRef, core::SymbolRef> &aliases) {
    /** Try to pay additional attention not to duplicate any part of tree.
     * Though this may lead to more effictient and a better CFG if it was to be actually compiled into code
     * This will lead to duplicate typechecking and may lead to exponential explosion of typechecking time
     * for some code snippets. */
    Error::check(!current->bexit.cond.exists());

    BasicBlock *ret = nullptr;
    typecase(what,
             [&](ast::While *a) {
                 auto headerBlock = inWhat.freshBlock(loops + 1);
                 unconditionalJump(current, headerBlock, inWhat);

                 core::SymbolRef condSym =
                     ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::whileTemp(), inWhat.symbol);
                 auto headerEnd = walk(ctx, a->cond.get(), headerBlock, inWhat, condSym, loops + 1, aliases);
                 auto bodyBlock = inWhat.freshBlock(loops + 1);
                 auto continueBlock = inWhat.freshBlock(loops);
                 conditionalJump(headerEnd, condSym, bodyBlock, continueBlock, inWhat);
                 // finishHeader
                 core::SymbolRef bodySym =
                     ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::statTemp(), inWhat.symbol);

                 auto body = walk(ctx, a->body.get(), bodyBlock, inWhat, bodySym, loops + 1, aliases);
                 unconditionalJump(body, headerBlock, inWhat);

                 continueBlock->exprs.emplace_back(target, a->loc, make_unique<Nil>());
                 ret = continueBlock;
             },
             [&](ast::Return *a) {
                 core::SymbolRef retSym =
                     ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::returnTemp(), inWhat.symbol);
                 auto cont = walk(ctx, a->expr.get(), current, inWhat, retSym, loops, aliases);
                 cont->exprs.emplace_back(target, a->loc, make_unique<Return>(retSym)); // dead assign.
                 jumpToDead(cont, inWhat);
                 ret = deadBlock();
             },
             [&](ast::If *a) {
                 core::SymbolRef ifSym =
                     ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::ifTemp(), inWhat.symbol);
                 Error::check(ifSym.exists());
                 auto thenBlock = inWhat.freshBlock(loops);
                 auto elseBlock = inWhat.freshBlock(loops);
                 auto cont = walk(ctx, a->cond.get(), current, inWhat, ifSym, loops, aliases);
                 conditionalJump(cont, ifSym, thenBlock, elseBlock, inWhat);

                 auto thenEnd = walk(ctx, a->thenp.get(), thenBlock, inWhat, target, loops, aliases);
                 auto elseEnd = walk(ctx, a->elsep.get(), elseBlock, inWhat, target, loops, aliases);
                 if (thenEnd != deadBlock() || elseEnd != deadBlock()) {
                     if (thenEnd == deadBlock()) {
                         ret = elseEnd;
                     } else if (thenEnd == deadBlock()) {
                         ret = thenEnd;
                     } else {
                         ret = freshBlock(loops);
                         unconditionalJump(thenEnd, ret, inWhat);
                         unconditionalJump(elseEnd, ret, inWhat);
                     }
                 } else {
                     ret = deadBlock();
                 }
             },
             [&](ast::IntLit *a) {
                 current->exprs.emplace_back(target, a->loc, make_unique<IntLit>(a->value));
                 ret = current;
             },
             [&](ast::FloatLit *a) {
                 current->exprs.emplace_back(target, a->loc, make_unique<FloatLit>(a->value));
                 ret = current;
             },
             [&](ast::StringLit *a) {
                 current->exprs.emplace_back(target, a->loc, make_unique<StringLit>(a->value));
                 ret = current;
             },
             [&](ast::BoolLit *a) {
                 current->exprs.emplace_back(target, a->loc, make_unique<BoolLit>(a->value));
                 ret = current;
             },
             [&](ast::ConstantLit *a) { Error::raise("Should have been eliminated by namer/resolver"); },
             [&](ast::Ident *a) {
                 current->exprs.emplace_back(target, a->loc,
                                             make_unique<Ident>(global2Local(ctx, a->symbol, inWhat, aliases)));
                 ret = current;
             },
             [&](ast::Self *a) {
                 current->exprs.emplace_back(target, a->loc, make_unique<Self>(a->claz));
                 ret = current;
             },
             [&](ast::Assign *a) {
                 auto lhsIdent = dynamic_cast<ast::Ident *>(a->lhs.get());
                 core::SymbolRef lhs;
                 if (lhsIdent != nullptr) {
                     lhs = global2Local(ctx, lhsIdent->symbol, inWhat, aliases);
                 } else {
                     // TODO(nelhage): Once namer is complete this should be a
                     // fatal error
                     lhs = ctx.state.defn_todo();
                 }
                 auto rhsCont = walk(ctx, a->rhs.get(), current, inWhat, lhs, loops, aliases);
                 rhsCont->exprs.emplace_back(target, a->loc, make_unique<Ident>(lhs));
                 ret = rhsCont;
             },
             [&](ast::InsSeq *a) {
                 for (auto &exp : a->stats) {
                     core::SymbolRef temp =
                         ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::statTemp(), inWhat.symbol);
                     current = walk(ctx, exp.get(), current, inWhat, temp, loops, aliases);
                 }
                 ret = walk(ctx, a->expr.get(), current, inWhat, target, loops, aliases);
             },
             [&](ast::Send *s) {
                 core::SymbolRef recv;

                 recv = ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::statTemp(), inWhat.symbol);
                 current = walk(ctx, s->recv.get(), current, inWhat, recv, loops, aliases);

                 vector<core::SymbolRef> args;
                 for (auto &exp : s->args) {
                     core::SymbolRef temp;
                     temp = ctx.state.newTemporary(core::UniqueNameKind::CFG, core::Names::statTemp(), inWhat.symbol);
                     current = walk(ctx, exp.get(), current, inWhat, temp, loops, aliases);

                     args.push_back(temp);
                 }

                 if (s->block != nullptr) {
                     auto headerBlock = inWhat.freshBlock(loops + 1);
                     auto postBlock = inWhat.freshBlock(loops);
                     auto bodyBlock = inWhat.freshBlock(loops + 1);

                     for (int i = 0; i < s->block->args.size(); ++i) {
                         auto &arg = s->block->args[i];

                         if (auto id = dynamic_cast<ast::Ident *>(arg.get())) {
                             bodyBlock->exprs.emplace_back(id->symbol, arg->loc, make_unique<LoadArg>(recv, s->fun, i));
                         } else {
                             // TODO(nelhage): this will be an error once the namer
                             // is more complete and turns all args into Ident
                         }
                     }

                     conditionalJump(headerBlock, ctx.state.defn_cfg_block_call(), bodyBlock, postBlock, inWhat);

                     unconditionalJump(current, headerBlock, inWhat);

                     // TODO: handle block arguments somehow??
                     core::SymbolRef blockrv = ctx.state.newTemporary(core::UniqueNameKind::CFG,
                                                                      core::Names::blockReturnTemp(), inWhat.symbol);
                     auto blockLast = walk(ctx, s->block->body.get(), bodyBlock, inWhat, blockrv, loops + 1, aliases);

                     unconditionalJump(blockLast, headerBlock, inWhat);

                     current = postBlock;
                 }

                 current->exprs.emplace_back(target, s->loc, make_unique<Send>(recv, s->fun, args));
                 ret = current;
             },

             [&](ast::Expression *n) {
                 current->exprs.emplace_back(target, n->loc, make_unique<NotSupported>(""));
                 ret = current;
             },

             [&](ast::Block *a) { Error::raise("should never encounter a bare Block"); });

    /*[&](ast::Break *a) {}, [&](ast::Next *a) {},*/
    // For, Next, Rescue,
    // Symbol, Send, New, Super, NamedArg, Hash, Array,
    // ArraySplat, HashAplat, Block,
    Error::check(ret != nullptr);
    return ret;
}

string CFG::toString(core::Context ctx) {
    stringstream buf;
    buf << "subgraph \"cluster_" << this->symbol.info(ctx).fullName(ctx) << "\" {" << endl;
    buf << "    label = \"" << this->symbol.info(ctx).fullName(ctx) << "\";" << endl;
    buf << "    color = blue;" << endl;
    buf << "    bb" << this->symbol._id << "_0 [shape = invhouse];" << endl;
    buf << "    bb" << this->symbol._id << "_1 [shape = parallelogram];" << endl << endl;
    for (int i = 0; i < this->basicBlocks.size(); i++) {
        auto text = this->basicBlocks[i]->toString(ctx);
        buf << "    bb" << this->symbol._id << "_" << i << " [label = \"" << text << "\"];" << endl;
        auto thenI = find_if(this->basicBlocks.begin(), this->basicBlocks.end(),
                             [&](auto &a) { return a.get() == this->basicBlocks[i]->bexit.thenb; });
        auto elseI = find_if(this->basicBlocks.begin(), this->basicBlocks.end(),
                             [&](auto &a) { return a.get() == this->basicBlocks[i]->bexit.elseb; });
        buf << "    bb" << this->symbol._id << "_" << i << " -> bb" << this->symbol._id << "_"
            << thenI - this->basicBlocks.begin() << ";" << endl;
        if (this->basicBlocks[i]->bexit.cond != ctx.state.defn_cfg_always() &&
            this->basicBlocks[i]->bexit.cond != ctx.state.defn_cfg_never()) {
            buf << "    bb" << this->symbol._id << "_" << i << " -> bb" << this->symbol._id << "_"
                << elseI - this->basicBlocks.begin() << ";" << endl
                << endl;
        }
    }
    buf << "}";
    return buf.str();
}

string BasicBlock::toString(core::Context ctx) {
    stringstream buf;
    buf << "(";
    bool first = true;
    for (core::SymbolRef arg : this->args) {
        if (!first) {
            buf << ", ";
        }
        first = false;
        buf << arg.info(ctx).name.name(ctx).toString(ctx);
    }
    buf << ")\\n";
    if (this->outerLoops > 0) {
        buf << "outerLoops: " << this->outerLoops << "\\n";
    }
    for (Binding &exp : this->exprs) {
        buf << exp.bind.info(ctx).name.name(ctx).toString(ctx) << " = " << exp.value->toString(ctx);
        if (exp.tpe) {
            buf << " : " << Strings::escapeCString(exp.tpe->toString(ctx));
        }
        buf << "\\n"; // intentional! graphviz will do interpolation.
    }
    buf << this->bexit.cond.info(ctx).name.name(ctx).toString(ctx);
    return buf.str();
}

Binding::Binding(core::SymbolRef bind, core::Loc loc, unique_ptr<Instruction> value)
    : bind(bind), loc(loc), value(move(value)) {}

Return::Return(core::SymbolRef what) : what(what) {}

string Return::toString(core::Context ctx) {
    return "return " + this->what.info(ctx).name.name(ctx).toString(ctx);
}

Send::Send(core::SymbolRef recv, core::NameRef fun, vector<core::SymbolRef> &args)
    : recv(recv), fun(fun), args(move(args)) {}

Super::Super(vector<core::SymbolRef> &args) : args(move(args)) {}

string Super::toString(core::Context ctx) {
    stringstream buf;
    buf << "super(";
    bool isFirst = true;
    for (auto arg : this->args) {
        if (!isFirst) {
            buf << ", ";
        }
        isFirst = false;
        buf << arg.info(ctx).name.name(ctx).toString(ctx);
    }
    buf << ")";
    return buf.str();
}

FloatLit::FloatLit(float value) : value(value) {}

string FloatLit::toString(core::Context ctx) {
    return to_string(this->value);
}

IntLit::IntLit(int value) : value(value) {}

string IntLit::toString(core::Context ctx) {
    return to_string(this->value);
}

Ident::Ident(core::SymbolRef what) : what(what) {}

Alias::Alias(core::SymbolRef what) : what(what) {}

string Ident::toString(core::Context ctx) {
    return this->what.info(ctx).name.name(ctx).toString(ctx);
}

string Alias::toString(core::Context ctx) {
    return "alias " + this->what.info(ctx).name.name(ctx).toString(ctx);
}

string Send::toString(core::Context ctx) {
    stringstream buf;
    buf << this->recv.info(ctx).name.name(ctx).toString(ctx) << "." << this->fun.name(ctx).toString(ctx) << "(";
    bool isFirst = true;
    for (auto arg : this->args) {
        if (!isFirst) {
            buf << ", ";
        }
        isFirst = false;
        buf << arg.info(ctx).name.name(ctx).toString(ctx);
    }
    buf << ")";
    return buf.str();
}

string StringLit::toString(core::Context ctx) {
    return this->value.name(ctx).toString(ctx);
}

string BoolLit::toString(core::Context ctx) {
    if (value) {
        return "true";
    } else {
        return "false";
    }
}

string Nil::toString(core::Context ctx) {
    return "nil";
}

string Self::toString(core::Context ctx) {
    return "self";
}

string LoadArg::toString(core::Context ctx) {
    stringstream buf;
    buf << "load_arg(";
    buf << this->receiver.info(ctx).name.name(ctx).toString(ctx);
    buf << "#";
    buf << this->method.name(ctx).toString(ctx);
    buf << ", " << this->arg << ")";
    return buf.str();
}

string NotSupported::toString(core::Context ctx) {
    return "NotSupported(" + why + ")";
}
} // namespace cfg
} // namespace ruby_typer
