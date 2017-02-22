#include <iomanip>
#include <sstream>
#include "slicing.h"
#include "slicingtree.h"
#include "chunk/dump.h"
#include "disasm/disassemble.h"
#include "log/log.h"

TreeNode *SearchState::getRegTree(int reg) {
    auto it = regTree.find(reg);
    return (it != regTree.end() ? (*it).second : nullptr);
}

void SearchState::setRegTree(int reg, TreeNode *tree) {
    regTree[reg] = tree;
}

const char *SlicingUtilities::printReg(int reg) {
    Disassemble::Handle handle(true);
    return cs_reg_name(handle.raw(), reg);
}

void SlicingUtilities::printRegs(SearchState *state, bool withNewline) {
    std::ostringstream output;
    output << "[";

    bool firstReg = true;
    const auto &regs = state->getRegs();
    for(size_t r = 0; r < regs.size(); r ++) {
        if(!regs[r]) continue;

        if(!firstReg) output << " ";
        firstReg = false;
        output << printReg(r);
    }
    output << "]";

    LOG0(1, "    regs " << std::left << std::setw(30)
        << output.str());

    if(withNewline) LOG(1, "");
}

void SlicingUtilities::printRegTrees(SearchState *state) {
    const auto &regs = state->getRegs();
    for(size_t r = 0; r < regs.size(); r ++) {
        auto tree = state->getRegTree(r);
        if(!tree) continue;

        std::cout << "        REG " << printReg(r) << ": ";
        tree->print(TreePrinter(3, 1));
        std::cout << "\n";
    }
}

void SlicingUtilities::copyParentRegTrees(SearchState *state) {
    const auto &regs = state->getRegs();
    for(size_t r = 0; r < regs.size(); r ++) {
        if(regs[r] && !state->getRegTree(r)) {
            // didn't compute this register yet, copy from parent(s)
            state->setRegTree(r, getParentRegTree(state, r));
        }
    }
}

TreeNode *SlicingUtilities::makeMemTree(SearchState *state, x86_op_mem *mem) {
    TreeNode *tree = nullptr;
    if(mem->index != X86_REG_INVALID) {
        tree = getParentRegTree(state, mem->index);
        if(mem->scale != 1) {
            tree = new TreeNodeMultiplication(
                tree,
                new TreeNodeConstant(mem->scale));
        }
    }

    TreeNode *baseTree = getParentRegTree(state, mem->base);
    if(tree) {
        tree = new TreeNodeAddition(baseTree, tree);
    }
    else if(mem->base != X86_REG_INVALID) {
        tree = baseTree;
    }

    if(mem->disp) {
        tree = new TreeNodeAddition(
            new TreeNodeAddress(mem->disp), tree);
    }

    return tree;
}

TreeNode *SlicingUtilities::getParentRegTree(SearchState *state, int reg) {
    if(reg == X86_REG_RIP) {
        auto i = state->getInstruction();
        return new TreeNodeRegisterRIP(i->getAddress() + i->getSize());
    }

    const auto &parents = state->getParents();
    if(parents.size() == 0) {
        return new TreeNodeRegister(reg);
    }
    else if(parents.size() == 1) {
        auto tree = parents.front()->getRegTree(reg);
        if(!tree) tree = new TreeNodeRegister(reg);
        return tree;
    }
    else {
        auto tree = new TreeNodeMultipleParents();
        for(auto p : parents) {
            auto t = p->getRegTree(reg);
            if(!t) t = new TreeNodeRegister(reg);
            tree->addParent(t);
        }
        return tree;
    }
}

void SlicingSearch::sliceAt(Instruction *i) {
    auto block = dynamic_cast<Block *>(i->getParent());
    auto node = cfg->get(block);
    LOG(1, "begin slicing at " << i->getName());

    SearchState *startState = new SearchState(node, i);
    auto j = dynamic_cast<IndirectJumpInstruction *>(i->getSemantic());
    startState->addReg(j->getRegister());

    buildStatePass(startState);
    buildRegTreePass();
}

void SlicingSearch::buildStatePass(SearchState *startState) {
    // We perform a breadth-first search through parent CFG nodes
    std::vector<bool> visited(cfg->getCount());  // indexed by node ID
    std::vector<SearchState *> transitionList;  // new states (BFS)
    SearchState *currentState = nullptr;  // not in stateList or transitionList
    // generating this->stateList

    transitionList.push_back(startState);

    while(transitionList.size() > 0) {
        currentState = transitionList.front();
        transitionList.erase(transitionList.begin());  // inefficient
        auto node = currentState->getNode();
        Instruction *instruction = currentState->getInstruction();

        if(visited[node->getID()]) continue;
        visited[node->getID()] = true;

        LOG(1, "visit " << node->getDescription());

        // visit all prior instructions in this node in backwards order
        auto insList = node->getBlock()->getChildren()->getIterable();
        for(int index = insList->indexOf(instruction); index >= 0; index --) {
            Instruction *i = insList->get(index);
            ChunkDumper dumper;
            dumper.visit(i);

            currentState->setInstruction(i);

            buildStateFor(currentState);
            stateList.push_back(currentState);

            if(index > 0) {
                auto newState = new SearchState(*currentState);
                currentState->addParent(newState);
                currentState = newState;
            }
        }

        // find all nodes that link to this one, keep searching there
        for(auto link : node->backwardLinks()) {
            auto newNode = cfg->get(link.first);
            if(!visited[newNode->getID()]) {
                auto offset = link.second;
                Instruction *newStart
                    = newNode->getBlock()->getChildren()->getSpatial()->find(
                        newNode->getBlock()->getAddress() + offset);
                LOG(1, "    start at offset " << offset << " -> " << newStart);
                SearchState *newState = new SearchState(*currentState);
                newState->setNode(newNode);
                newState->setInstruction(newStart);
                transitionList.push_back(newState);
                currentState->addParent(newState);
            }
        }
    }
}

void SlicingSearch::buildRegTreePass() {
    LOG(1, "second pass iteration");
    for(auto it = stateList.rbegin(); it != stateList.rend(); ++it) {
        auto state = (*it);
        auto instruction = state->getInstruction();

        SlicingUtilities u;
        u.printRegs(state, false);
        ChunkDumper dumper;
        dumper.visit(instruction);

        buildRegTreesFor(state);
        u.copyParentRegTrees(state);
        u.printRegTrees(state);
    }
}

void SlicingSearch::debugPrintRegAccesses(Instruction *i) {
    auto capstone = i->getSemantic()->getCapstone();
    if(!capstone || !capstone->detail) return;
    auto detail = capstone->detail;

    SlicingUtilities u;

    for(size_t r = 0; r < detail->regs_read_count; r ++) {
        LOG(1, "        implicit reg read "
            << u.printReg(detail->regs_read[r]));
    }
    for(size_t r = 0; r < detail->regs_write_count; r ++) {
        LOG(1, "        implicit reg write "
            << u.printReg(detail->regs_write[r]));
    }

#ifdef ARCH_X86_64
    cs_x86 *x = &detail->x86;
#elif defined(ARCH_AARCH64)
    cs_arm64 *x = &detail->arm64;
#endif
    for(size_t p = 0; p < x->op_count; p ++) {
        auto op = &x->operands[p];  // cs_x86_op*, cs_arm64_op*
        if(static_cast<cs_op_type>(op->type) == CS_OP_REG) {
            LOG(1, "        explicit reg ref "
                << u.printReg(op->reg));
        }
    }
}

bool SlicingSearch::isKnownInstruction(unsigned id) {
    static bool known[X86_INS_ENDING] = {};
    known[X86_INS_ADD] = true;
    known[X86_INS_LEA] = true;
    known[X86_INS_MOVSXD] = true;

    return id < sizeof(known)/sizeof(*known) && known[id];
}

void SlicingSearch::buildStateFor(SearchState *state) {
    auto capstone = state->getInstruction()->getSemantic()->getCapstone();
    if(!capstone || !capstone->detail) return;

#ifdef ARCH_X86_64
    cs_x86 *x = &capstone->detail->x86;
#elif defined(ARCH_AARCH64)
    cs_arm64 *x = &capstone->detail->arm64;
#endif

    if(isKnownInstruction(capstone->id)) {
        if(x->op_count == 2
            && x->operands[0].type == X86_OP_REG
            && x->operands[1].type == X86_OP_REG) {

            auto source = x->operands[0].reg;
            auto target = x->operands[1].reg;

            if(state->getReg(target)) {
                state->addReg(source);
                state->addReg(target);
            }
        }
        if(x->op_count == 2
            && x->operands[0].type == X86_OP_MEM
            && x->operands[1].type == X86_OP_REG) {

            auto mem = &x->operands[0].mem;
            auto out = x->operands[1].reg;

            if(state->getReg(out)) {
                state->removeReg(out);
                if(mem->base != X86_REG_INVALID) {
                    state->addReg(mem->base);
                }
                if(mem->index != X86_REG_INVALID) {
                    state->addReg(mem->index);
                }
            }
        }
    }

    state->removeReg(X86_REG_RIP);  // never care about this
}

void SlicingSearch::buildRegTreesFor(SearchState *state) {
    auto capstone = state->getInstruction()->getSemantic()->getCapstone();
    if(!capstone || !capstone->detail) return;

    SlicingUtilities u;

    enum {
        MODE_UNKNOWN,
        MODE_REG_REG,
        MODE_MEM_REG,
    } mode = MODE_UNKNOWN;

#ifdef ARCH_X86_64
    cs_x86 *x = &capstone->detail->x86;
#elif defined(ARCH_AARCH64)
    cs_arm64 *x = &capstone->detail->arm64;
#endif
    if(isKnownInstruction(capstone->id)) {
        if(x->op_count == 2
            && x->operands[0].type == X86_OP_REG
            && x->operands[1].type == X86_OP_REG) {

            mode = MODE_REG_REG;
        }
        if(x->op_count == 2
            && x->operands[0].type == X86_OP_MEM
            && x->operands[1].type == X86_OP_REG) {

            mode = MODE_MEM_REG;
        }
    }

    switch(capstone->id) {
    case X86_INS_ADD:
        if(mode == MODE_REG_REG) {
            auto source = x->operands[0].reg;
            auto target = x->operands[1].reg;

            state->setRegTree(target, new TreeNodeAddition(
                u.getParentRegTree(state, source),
                u.getParentRegTree(state, target)));
        }
        LOG(1, "        add found");
        break;
    case X86_INS_LEA:
        if(mode == MODE_MEM_REG) {
            auto mem = &x->operands[0].mem;
            auto out = x->operands[1].reg;

            auto tree = u.makeMemTree(state, mem);
            state->setRegTree(out, tree);
        }
        LOG(1, "        lea found");
        break;
    case X86_INS_MOVSXD:
        if(x->operands[0].type == X86_OP_MEM
            && x->operands[1].type == X86_OP_REG) {

            auto mem = &x->operands[0].mem;
            auto out = x->operands[1].reg;

            auto tree = u.makeMemTree(state, mem);
            state->setRegTree(out, tree);
        }

        LOG(1, "        movslq found");
        break;
    default:
        LOG(1, "        got instr id " << capstone->id);
        break;
    }
}
