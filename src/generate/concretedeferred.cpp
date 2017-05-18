#include <cstring>  // for memset
#include "concretedeferred.h"
#include "section.h"
#include "sectionlist.h"
#include "elf/elfspace.h"
#include "elf/symbol.h"
#include "chunk/function.h"
#include "chunk/dataregion.h"
#include "chunk/link.h"
#include "instr/instr.h"
#include "instr/concrete.h"
#include "log/log.h"

bool SymbolInTable::operator < (const SymbolInTable &other) const {
    if(type < other.type) return true;
    if(type > other.type) return false;

#if 1
    // NULL symbols are ordered first
    bool null1 = (sym == nullptr);
    bool null2 = (other.get() == nullptr);
    if(null1 && !null2) return true;
    if(!null1 && null2) return false;
    if(null1 && null2) return false;  // shouldn't happen
#endif

#if 0
    auto address1 = sym->getAddress();
    auto address2 = other.get()->getAddress();
    if(!address1 && address2) return true;
    if(address1 && !address2) return false;
    if(address1 && address2) {
        if(address1 < address2) return true;
        if(address1 > address2) return false;
    }
#endif

    int nameCompare = std::strcmp(sym->getName(), other.get()->getName());
    if(nameCompare < 0) return true;
    if(nameCompare > 0) return false;

    int section1 = sym->getSectionIndex();
    int section2 = other.get()->getSectionIndex();

    return section1 < section2;
}

bool SymbolInTable::operator == (const SymbolInTable &other) const {
    // right now we never copy Symbols, so we can just compare addresses
    return sym == other.get();
}

std::string SymbolInTable::getName() const {
    return sym ? sym->getName() : "(null)";
}

void SymbolTableContent::addNullSymbol() {
    auto symbol = new ElfXX_Sym();
    symbol->st_name = strtab->add("", 1);  // add empty name
    symbol->st_info = 0;
    symbol->st_other = STV_DEFAULT;
    symbol->st_shndx = 0;
    symbol->st_value = 0;
    symbol->st_size = 0;
    auto sit = SymbolInTable(SymbolInTable::TYPE_NULL);
    insertSorted(sit, new DeferredType(symbol));
    firstGlobalIndex ++;
}

void SymbolTableContent::addSectionSymbol(Symbol *sym) {
    ElfXX_Sym *symbol = new ElfXX_Sym();
    symbol->st_name = 0;
    symbol->st_info = ELFXX_ST_INFO(
        Symbol::bindFromInternalToElf(sym->getBind()),
        Symbol::typeFromInternalToElf(sym->getType()));
    symbol->st_other = STV_DEFAULT;
    symbol->st_shndx = sym->getSectionIndex();
    symbol->st_value = 0;
    symbol->st_size = 0;

    LOG(1, "added section symbol for " << symbol->st_shndx);

    auto value = new DeferredType(symbol);
    auto sit = SymbolInTable(SymbolInTable::TYPE_SECTION, sym);
    insertSorted(sit, value);
    firstGlobalIndex ++;

    auto i = sym->getSectionIndex();
    if(i >= sectionSymbols.size()) sectionSymbols.resize(i + 1);
    sectionSymbols[i] = value;
}

SymbolTableContent::DeferredType *SymbolTableContent
    ::addSymbol(Function *func, Symbol *sym) {

    auto name = std::string(sym->getName());
    auto index = strtab->add(name, true);  // add name to string table

    ElfXX_Sym *symbol = new ElfXX_Sym();
    symbol->st_name = static_cast<ElfXX_Word>(index);
    symbol->st_info = ELFXX_ST_INFO(
        Symbol::bindFromInternalToElf(sym->getBind()),
        Symbol::typeFromInternalToElf(sym->getType()));
    symbol->st_other = STV_DEFAULT;
    symbol->st_shndx = SHN_UNDEF;
    symbol->st_value = func ? func->getAddress() : 0;
    symbol->st_size = func ? func->getSize() : 0;
    auto value = new DeferredType(symbol);
    if(sym->getBind() == Symbol::BIND_LOCAL) {
        auto sit = SymbolInTable(SymbolInTable::TYPE_LOCAL, sym);
        insertSorted(sit, value);
        firstGlobalIndex ++;
    }
    else {
        auto sit = SymbolInTable(func
            ? SymbolInTable::TYPE_GLOBAL
            : SymbolInTable::TYPE_UNDEF, sym);
        insertSorted(sit, value);
    }
    return value;
}

SymbolTableContent::DeferredType *SymbolTableContent
    ::addUndefinedSymbol(Symbol *sym) {

    // uses st_shndx = SHN_UNDEF by default
    return addSymbol(nullptr, sym);
}

size_t SymbolTableContent::indexOfSectionSymbol(const std::string &section,
    SectionList *sectionList) {

    size_t index = sectionList->indexOf(section);
    return this->indexOf(sectionSymbols[index]);
}

ShdrTableContent::DeferredType *ShdrTableContent::add(Section *section) {
    auto shdr = new ElfXX_Shdr();
    std::memset(shdr, 0, sizeof(*shdr));

    auto deferred = new DeferredType(shdr);

    deferred->addFunction([this, section] (ElfXX_Shdr *shdr) {
        LOG(1, "generating shdr for section [" << section->getName() << "]");
        auto header = section->getHeader();
        //shdr->sh_name       = 0;
        shdr->sh_type       = header->getShdrType();
        shdr->sh_flags      = header->getShdrFlags();
        shdr->sh_addr       = header->getAddress();
        shdr->sh_offset     = section->getOffset();
        shdr->sh_size       = section->getContent() ?
            section->getContent()->getSize() : 0;
        shdr->sh_link       = header->getSectionLink()
            ? header->getSectionLink()->getIndex() : 0;
        shdr->sh_info       = 0;  // updated later for strtabs
        shdr->sh_addralign  = 1;
        shdr->sh_entsize    = 0;
    });

    DeferredMap<Section *, ElfXX_Shdr>::add(section, deferred);
    return deferred;
}

Section *RelocSectionContent::getTargetSection() {
    return outer->get();
}

RelocSectionContent::DeferredType *RelocSectionContent
    ::add(Chunk *source, Link *link) {

    if(auto v = dynamic_cast<DataOffsetLink *>(link)) {
        return addConcrete(dynamic_cast<Instruction *>(source), v);
    }
    else if(auto v = dynamic_cast<PLTLink *>(link)) {
        return addConcrete(dynamic_cast<Instruction *>(source), v);
    }
    else if(auto v = dynamic_cast<SymbolOnlyLink *>(link)) {
        return addConcrete(dynamic_cast<Instruction *>(source), v);
    }

    return nullptr;
}

RelocSectionContent::DeferredType *RelocSectionContent
    ::makeDeferredForLink(Instruction *source) {

    auto rela = new ElfXX_Rela();
    std::memset(rela, 0, sizeof(*rela));
    auto deferred = new DeferredType(rela);

    auto address = source->getAddress();
    int specialAddendOffset = 0;
#ifdef ARCH_X86_64
    if(auto sem = dynamic_cast<LinkedInstruction *>(source->getSemantic())) {
        address += sem->getDispOffset();
        specialAddendOffset = -(sem->getSize() - sem->getDispOffset());
    }
    else if(auto sem = dynamic_cast<ControlFlowInstruction *>(source->getSemantic())) {
        address += sem->getDispOffset();
        specialAddendOffset = -(sem->getSize() - sem->getDispOffset());
    }
#elif defined(ARCH_AARCH64)
#else
    #error "how do we encode relocation offsets in instructions on arm?"
#endif

    rela->r_offset  = address;
    rela->r_info    = 0;
    rela->r_addend  = specialAddendOffset;

    DeferredMap<address_t, ElfXX_Rela>::add(address, deferred);
    return deferred;
}

RelocSectionContent::DeferredType *RelocSectionContent
    ::addConcrete(Instruction *source, DataOffsetLink *link) {

    auto deferred = makeDeferredForLink(source);
    auto rela = deferred->getElfPtr();

    auto dest = static_cast<DataRegion *>(&*link->getTarget());  // assume != nullptr
    auto destAddress = link->getTargetAddress();

    rela->r_addend += destAddress - dest->getAddress();
    auto rodata = elfSpace->getElfMap()->findSection(".rodata")->getHeader();
    rela->r_addend -= rodata->sh_offset;  // offset of original .rodata

    auto symtab = (*sectionList)[".symtab"]->castAs<SymbolTableContent *>();
    deferred->addFunction([this, symtab] (ElfXX_Rela *rela) {
        size_t index = symtab->indexOfSectionSymbol(".rodata", sectionList);
        rela->r_info = ELFXX_R_INFO(index, R_X86_64_PC32);
    });

    return deferred;
}

RelocSectionContent::DeferredType *RelocSectionContent
    ::addConcrete(Instruction *source, PLTLink *link) {

    auto deferred = makeDeferredForLink(source);

    auto symtab = (*sectionList)[".symtab"]->castAs<SymbolTableContent *>();
    deferred->addFunction([this, symtab, link] (ElfXX_Rela *rela) {
        auto sit = SymbolInTable(SymbolInTable::TYPE_UNDEF,
            link->getPLTTrampoline()->getTargetSymbol());
        auto elfSym = symtab->find(sit);
        size_t index = symtab->indexOf(elfSym);
        if(index == (size_t)-1) LOG(1, "ERROR with target "
            << link->getPLTTrampoline()->getTargetSymbol()->getName());
        rela->r_info = ELFXX_R_INFO(index, R_X86_64_PLT32);
    });

    return deferred;
}

RelocSectionContent::DeferredType *RelocSectionContent
    ::addConcrete(Instruction *source, SymbolOnlyLink *link) {

    auto deferred = makeDeferredForLink(source);

    auto symtab = (*sectionList)[".symtab"]->castAs<SymbolTableContent *>();
    deferred->addFunction([this, symtab, link] (ElfXX_Rela *rela) {
        auto sit = SymbolInTable(SymbolInTable::TYPE_UNDEF,
            link->getSymbol());
        auto elfSym = symtab->find(sit);
        size_t index = symtab->indexOf(elfSym);
        rela->r_info = ELFXX_R_INFO(index, R_X86_64_PLT32);
    });

    return deferred;
}