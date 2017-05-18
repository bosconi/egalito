#ifndef EGALITO_GENERATE_CONCRETE_DEFERRED_H
#define EGALITO_GENERATE_CONCRETE_DEFERRED_H

#include <vector>
#include "deferred.h"
#include "chunk/link.h"
#include "elf/elfxx.h"

class Section;
class SectionRef;
class Symbol;
class Function;
class Instruction;
class ElfSpace;
class Chunk;
class SectionList;

class SymbolInTable {
public:
    enum type_t {
        TYPE_NULL,
        TYPE_SECTION,
        TYPE_LOCAL,
        TYPE_UNDEF,
        TYPE_GLOBAL
    };
private:
    type_t type;
    Symbol *sym;
public:
    SymbolInTable(type_t type = TYPE_NULL, Symbol *sym = nullptr)
        : type(type), sym(sym) {}
    bool operator < (const SymbolInTable &other) const;
    bool operator == (const SymbolInTable &other) const;
    Symbol *get() const { return sym; }
    std::string getName() const;
};

/** Symbol table, either .strtab or .dynstr. The ordering of symbols
    is determined by the symbolCompare function.
*/
class SymbolTableContent : public DeferredMap<SymbolInTable, ElfXX_Sym> {
public:
    typedef DeferredValueImpl<ElfXX_Sym> DeferredType;
private:
    DeferredStringList *strtab;
    std::vector<DeferredType *> sectionSymbols;
    int firstGlobalIndex;
public:
    SymbolTableContent(DeferredStringList *strtab)
        : strtab(strtab), firstGlobalIndex(0) {}

    void addNullSymbol();
    void addSectionSymbol(Symbol *sym);
    DeferredType *addSymbol(Function *func, Symbol *sym);
    DeferredType *addUndefinedSymbol(Symbol *sym);

    size_t indexOfSectionSymbol(const std::string &section,
        SectionList *sectionList);
    int getFirstGlobalIndex() const { return firstGlobalIndex; }
};

class ShdrTableContent : public DeferredMap<Section *, ElfXX_Shdr> {
public:
    typedef DeferredValueImpl<ElfXX_Shdr> DeferredType;
public:
    DeferredType *add(Section *section);
};

class RelocSectionContent : public DeferredMap<address_t, ElfXX_Rela> {
public:
    typedef DeferredValueImpl<ElfXX_Rela> DeferredType;
private:
    SectionRef *outer;
    SectionList *sectionList;
    ElfSpace *elfSpace;
public:
    RelocSectionContent(SectionRef *outer, SectionList *sectionList,
        ElfSpace *elfSpace) : outer(outer), sectionList(sectionList),
        elfSpace(elfSpace) {}

    Section *getTargetSection();

    DeferredType *add(Chunk *source, Link *link);
private:
    DeferredType *makeDeferredForLink(Instruction *source);
    DeferredType *addConcrete(Instruction *source, DataOffsetLink *link);
    DeferredType *addConcrete(Instruction *source, PLTLink *link);
    DeferredType *addConcrete(Instruction *source, SymbolOnlyLink *link);
};

#endif