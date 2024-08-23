#include "GEPTracker.h"
#include "OperandUtils.h"
#include "includes.h"
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/ErrorHandling.h>
#include <optional>

namespace BinaryOperations {
  void* file_base_g;
  ZyanU8* data_g;

  void initBases(void* file_base, ZyanU8* data) {
    file_base_g = file_base;
    data_g = data;
  }

  void getBases(void** file_base, ZyanU8** data) {
    *file_base = file_base_g;
    *data = data_g;
  }

  const char* getName(uint64_t offset) {
    auto dosHeader = (win::dos_header_t*)file_base_g;
    auto ntHeaders =
        (win::nt_headers_x64_t*)((uint8_t*)file_base_g + dosHeader->e_lfanew);
    auto rvaOffset = FileHelper::RvaToFileOffset(ntHeaders, offset);
    return (const char*)file_base_g + rvaOffset;
  }
  bool isImport(uint64_t addr) {
    APInt tmp;
    auto dosHeader = (win::dos_header_t*)file_base_g;
    auto ntHeaders =
        (win::nt_headers_x64_t*)((uint8_t*)file_base_g + dosHeader->e_lfanew);
    return readMemory(ntHeaders->optional_header.image_base + addr, 1, tmp);
  }

  unordered_set<uint64_t> MemWrites;

  bool isWrittenTo(uint64_t addr) {
    return MemWrites.find(addr) != MemWrites.end();
  }
  void WriteTo(uint64_t addr) { MemWrites.insert(addr); }

  // sections
  bool readMemory(uint64_t addr, unsigned byteSize, APInt& value) {

    uint64_t mappedAddr =
        FileHelper::address_to_mapped_address(file_base_g, addr);
    uint64_t tempValue;

    if (mappedAddr > 0) {
      std::memcpy(&tempValue,
                  reinterpret_cast<const void*>(data_g + mappedAddr), byteSize);

      APInt readValue(byteSize * 8, tempValue);
      value = readValue;
      return 1;
    }

    return 0;
  }

  // TODO
  // 1- if writes into execute section, flag that address, if we execute that
  // address then do fancy stuff to figure out what we wrote so we know what
  // we will be executing
  void writeMemory();

}; // namespace BinaryOperations

void lifterMemoryBuffer::addValueReference(Instruction* inst, Value* value,
                                           uint64_t address) {
  unsigned valueSizeInBytes = value->getType()->getIntegerBitWidth() / 8;
  for (unsigned i = 0; i < valueSizeInBytes; i++) {

    delete buffer[address + i];
    BinaryOperations::WriteTo(address + i);
    printvalue2(address + i);
    buffer[address + i] = new ValueByteReference(inst, value, i);
    printvalue(value);
    printvalue2((uint64_t)address + i);
  }
}

void lifterMemoryBuffer::updateValueReference(Instruction* inst, Value* value,
                                              uint64_t address) {
  unsigned valueSizeInBytes = value->getType()->getIntegerBitWidth() / 8;
  for (unsigned i = 0; i < valueSizeInBytes; i++) {
    auto existingValue = buffer[address + i];
    auto DT = GEPStoreTracker::getDomTree();

    if (comesBefore(inst, existingValue->storeInst, *DT)) {
      continue;
    }

    printvalue2(address + i);

    buffer[address + i] = new ValueByteReference(inst, value, i);

    printvalue(value);

    printvalue2((uint64_t)address + i);
  }
}

Value* lifterMemoryBuffer::retrieveCombinedValue(IRBuilder<>& builder,
                                                 uint64_t startAddress,
                                                 uint64_t byteCount,
                                                 Value* orgLoad) {
  LLVMContext& context = builder.getContext();
  if (byteCount == 0) {
    return nullptr;
  }

  // bool contiguous = true;

  vector<ValueByteReferenceRange> values; // we can just create an array here
  for (uint64_t i = 0; i < byteCount; ++i) {
    uint64_t currentAddress = startAddress + i;
    if (buffer[currentAddress] == nullptr ||
        buffer[currentAddress]->value != buffer[startAddress]->value ||
        buffer[currentAddress]->byteOffset != i) {
      // contiguous = false; // non-contiguous value
    }

    // push if
    if (values.empty() ||                                 // empty or
        (buffer[currentAddress] && values.back().isRef && // ( its a reference
         (values.back().valinfo.ref->value !=
              buffer[currentAddress]->value || // and references are not same or
          values.back().valinfo.ref->byteOffset !=
              buffer[currentAddress]->byteOffset - values.back().end +
                  values.back().start)) //  reference offset is not directly
                                        //  next value )
    ) {

      if (buffer[currentAddress]) {
        values.push_back(
            ValueByteReferenceRange(buffer[currentAddress], i, i + 1));
      } else {
        values.push_back(ValueByteReferenceRange(currentAddress, i, i + 1));
      }
    } else {
      ++values.back().end;
    }
  }

  // if value is contiguous and value exists but we are trying to load a
  // truncated value
  // no need for this ?
  /*
  if (contiguous && buffer[startAddress] &&
      byteCount <=
          buffer[startAddress]->value->getType()->getIntegerBitWidth() / 8) {
    return builder.CreateTrunc(buffer[startAddress]->value,
                               Type::getIntNTy(context, byteCount * 8)); // ?
  }
  */

  // when do we want to return nullptr and when do we want to return 0?
  // we almost always want to return a value
  Value* result = ConstantInt::get(Type::getIntNTy(context, byteCount * 8), 0);

  int m = 0;
  for (auto v : values) {
    Value* byteValue = nullptr;
    unsigned bytesize = v.end - v.start;

    APInt mem_value(1, 0);
    if (v.isRef && v.valinfo.ref != nullptr) {
      byteValue =
          extractBytes(builder, v.valinfo.ref->value, v.valinfo.ref->byteOffset,
                       v.valinfo.ref->byteOffset + bytesize);
    } else if (!v.isRef && BinaryOperations::readMemory(v.valinfo.memoryAddress,
                                                        bytesize, mem_value)) {
      byteValue = builder.getIntN(bytesize * 8, mem_value.getZExtValue());
    } else if (!v.isRef) {
      // llvm_unreachable_internal("uh...");
      byteValue = extractBytes(builder, orgLoad, m, m + bytesize);
    }
    if (byteValue) {
      printvalue(byteValue);

      Value* shiftedByteValue = createShlFolder(
          builder,
          createZExtFolder(builder, byteValue,
                           Type::getIntNTy(context, byteCount * 8)),
          APInt(byteCount * 8, m * 8));
      result = createOrFolder(builder, result, shiftedByteValue,
                              "extractbytesthing");
    }
    m += bytesize;
  }
  return result;
}

Value* lifterMemoryBuffer::extractBytes(IRBuilder<>& builder, Value* value,
                                        uint64_t startOffset,
                                        uint64_t endOffset) {
  LLVMContext& context = builder.getContext();

  if (!value) {
    return ConstantInt::get(
        Type::getIntNTy(context, (endOffset - startOffset) * 8), 0);
  }

  uint64_t byteCount = endOffset - startOffset;

  uint64_t shiftAmount = startOffset * 8;

  printvalue2(endOffset);

  printvalue2(startOffset);
  printvalue2(byteCount);
  printvalue2(shiftAmount);

  Value* shiftedValue = createLShrFolder(
      builder, value,
      APInt(value->getType()->getIntegerBitWidth(), shiftAmount),
      "extractbytes");
  printvalue(value);
  printvalue(shiftedValue);

  Value* truncatedValue = createTruncFolder(
      builder, shiftedValue, Type::getIntNTy(context, byteCount * 8));
  return truncatedValue;
}

namespace SCCPSimplifier {
  std::unique_ptr<SCCPSolver> solver;
  uint64_t lastinstcount = 0;
  void init(Function* function) {
    if (function->getInstructionCount() == lastinstcount)
      return;
    lastinstcount = function->getInstructionCount();
    auto GetTLI = [](Function& F) -> const TargetLibraryInfo& {
      static TargetLibraryInfoImpl TLIImpl(
          Triple(F.getParent()->getTargetTriple()));
      static TargetLibraryInfo TLI(TLIImpl);
      return TLI;
    };

    solver = std::make_unique<SCCPSolver>(
        function->getParent()->getDataLayout(), GetTLI, function->getContext());
    solver->markBlockExecutable(&(function->front()));

    for (Argument& AI : function->args())
      solver->markOverdefined(&AI);
    bool ResolvedUndefs = true;
    while (ResolvedUndefs) {
      solver->solve();
      ResolvedUndefs = solver->resolvedUndefsIn(*function);
    }
  }
  SCCPSolver* get() { return solver.get(); }

  void cleanup() { solver.reset(); }
} // namespace SCCPSimplifier

// do some cleanup
// rename it to MemoryTracker ?
namespace GEPStoreTracker {
  DominatorTree* DT;
  BasicBlock* lastBB = nullptr;

  // best to use whenever possible
  lifterMemoryBuffer VirtualStack;

  void initDomTree(Function& F) { DT = new DominatorTree(F); }
  DominatorTree* getDomTree() { return DT; }

  void updateDomTree(Function& F) {
    // doesnt make a much difference, but good to have
    auto getLastBB = &(F.back());
    if (getLastBB != lastBB)
      DT->recalculate(F);
    lastBB = getLastBB;
  }

  vector<Instruction*> memInfos;
  void updateMemoryOp(StoreInst* inst) {

    auto ptr = inst->getPointerOperand();
    if (!isa<GetElementPtrInst>(ptr))
      return;

    auto gepInst = cast<GetElementPtrInst>(ptr);
    auto gepPtr = gepInst->getPointerOperand();
    if (gepPtr != getMemory())
      return;

    auto gepOffset = gepInst->getOperand(1);
    if (!isa<ConstantInt>(gepOffset))
      return;

    auto gepOffsetCI = cast<ConstantInt>(gepOffset);

    VirtualStack.updateValueReference(inst, inst->getValueOperand(),
                                      gepOffsetCI->getZExtValue());
  }

  map<uint64_t, uint64_t> pageMap;

  void markMemPaged(uint64_t start, uint64_t end) {
    //
    pageMap[start] = end;
  }

  bool isMemPaged(uint64_t address) {
    // ideally we want to be able to do this with KnownBits aswell
    auto it = pageMap.upper_bound(address);
    if (it == pageMap.begin())
      return false;
    --it;
    return address >= it->first && address < it->second;
  }

  enum isPaged { MEMORY_PAGED, MEMORY_MIGHT_BE_PAGED, MEMORY_NOT_PAGED };

  isPaged isValuePaged(Value* address, Instruction* ctxI) {
    if (isa<ConstantInt>(address)) {
      return isMemPaged(cast<ConstantInt>(address)->getZExtValue())
                 ? MEMORY_PAGED
                 : MEMORY_NOT_PAGED;
    }
    auto KBofAddress = analyzeValueKnownBits(address, ctxI);

    for (const auto& page : pageMap) {
      uint64_t start = page.first;
      uint64_t end = page.second;
      // KBofAddress >= start && KBofAddress < end
      // paged
      // but if we cant say otherwise, then it might be paged

      auto KBstart = KnownBits::makeConstant(APInt(64, start));
      auto KBend = KnownBits::makeConstant(APInt(64, end));

      if (KnownBits::uge(KBofAddress, KBstart) &&
          KnownBits::ult(KBofAddress, KBend)) {
        return MEMORY_PAGED;
      }

      if (!(KnownBits::uge(KBofAddress, KBend) ||
            KnownBits::ult(KBofAddress, KBstart))) {
        return MEMORY_MIGHT_BE_PAGED;
      }
    }

    return MEMORY_NOT_PAGED;
  }

  void pagedCheck(Value* address, Instruction* ctxI) {
    isPaged paged = isValuePaged(address, ctxI);

    switch (paged) {
    case MEMORY_NOT_PAGED: {
      printvalueforce(address);
      llvm_unreachable_internal(
          "\nmemory is not paged, so we(more likely) or the program "
          "probably do some incorrect stuff "
          "we abort to avoid incorrect output\n");
      cout.flush();
      break;
    }
    case MEMORY_MIGHT_BE_PAGED: {
      // something something if flag turned on print some data
      break;
    }
    case MEMORY_PAGED: {
      // nothing
      break;
    }
    }
  }

  void loadMemoryOp(LoadInst* inst) {
    auto ptr = inst->getPointerOperand();
    if (!isa<GetElementPtrInst>(ptr))
      return;

    auto gepInst = cast<GetElementPtrInst>(ptr);
    auto gepPtr = gepInst->getPointerOperand();
    if (gepPtr != getMemory())
      return;

    auto gepOffset = gepInst->getOperand(1);

    pagedCheck(gepOffset, inst);
    return;
  }

  // rename func name to indicate its only for store
  void insertMemoryOp(StoreInst* inst) {
    memInfos.push_back(inst);

    auto ptr = inst->getPointerOperand();
    if (!isa<GetElementPtrInst>(ptr))
      return;

    auto gepInst = cast<GetElementPtrInst>(ptr);
    auto gepPtr = gepInst->getPointerOperand();
    if (gepPtr != getMemory())
      return;

    auto gepOffset = gepInst->getOperand(1);

    pagedCheck(gepOffset, inst);

    if (!isa<ConstantInt>(gepOffset)) // we also want to do operations with the
                                      // memory when we can assume a range or
                                      // writing to an unk location (ofc paged)
      return;

    auto gepOffsetCI = cast<ConstantInt>(gepOffset);

    VirtualStack.addValueReference(inst, inst->getValueOperand(),
                                   gepOffsetCI->getZExtValue());
    BinaryOperations::WriteTo(gepOffsetCI->getZExtValue());
  }

  bool overlaps(uint64_t addr1, uint64_t size1, uint64_t addr2,
                uint64_t size2) {
    return std::max(addr1, addr2) < std::min(addr1 + size1, addr2 + size2);
  }

  uint64_t createmask(uint64_t a1, uint64_t a2, uint64_t b1, uint64_t b2) {

    auto start_overlap = max(a1, b1);
    auto end_overlap = min(a2, b2);
    int64_t diffStart = a1 - b1;

    printvalue2(start_overlap) printvalue2(end_overlap);
    // If there is no overlap
    if (start_overlap > end_overlap)
      return 0;

    auto num_bytes = end_overlap - start_overlap;
    // mask =>
    uint64_t mask = 0xffffffffffffffff >>
                    (64 - (num_bytes * 8)); // adjust mask for bytesize
    printvalue2(diffStart);
    if (diffStart <= 0)
      return mask;

    auto diffShift = abs(diffStart);

    printvalue2(mask) mask <<= (diffShift) * 8; // get the shifted mask
    printvalue2(mask)

        mask ^= -(diffStart < 0); // if diff was -, get the negative of mask
    printvalue2(mask)

        return mask;
  }

  struct PairHash {
    std::size_t operator()(const std::pair<llvm::Value*, int>& pair) const {
      // Combine the hashes of the two elements
      return hash<llvm::Value*>{}(pair.first) ^ hash<int>{}(pair.second);
    }
  };

  void removeDuplicateOffsets(vector<Instruction*>& vec) {
    if (vec.empty())
      return;

    unordered_map<pair<Value*, int>, Instruction*, PairHash> latestOffsets;
    vector<Instruction*> uniqueInstructions;
    uniqueInstructions.reserve(
        vec.size()); // reserve space assuming all could be unique
    latestOffsets.reserve(
        vec.size()); // reserve space assuming all could be unique

    for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
      auto inst = cast<StoreInst>(*it);
      auto GEPval = inst->getPointerOperand();
      auto valOp = inst->getValueOperand();
      int size = valOp->getType()->getIntegerBitWidth();
      auto GEPInst = cast<GetElementPtrInst>(GEPval);
      auto offset = GEPInst->getOperand(1);
      auto pair = make_pair(offset, size);

      if (latestOffsets.emplace(pair, *it).second) {
        uniqueInstructions.push_back(*it);
      }
    }

    vec.assign(uniqueInstructions.rbegin(), uniqueInstructions.rend());
  }

  void removeFutureInsts(vector<Instruction*>& vec, LoadInst* load) {
    // binary search
    auto it = std::lower_bound(
        vec.begin(), vec.end(), load,
        [](Instruction* a, Instruction* b) { return comesBefore(a, b, *DT); });

    if (it != vec.end()) {
      vec.erase(it, vec.end());
    }
  }

  set<APInt, APIntComparator> getPossibleValues(const llvm::KnownBits& known,
                                                unsigned max_unknown) {
    llvm::APInt base = known.One;
    llvm::APInt unknowns = ~(known.Zero | known.One);
    unsigned numBits = known.getBitWidth();

    set<APInt, APIntComparator> values;

    llvm::APInt combo(unknowns.getBitWidth(), 0);
    for (uint64_t i = 0; i < (1ULL << max_unknown); ++i) {
      llvm::APInt temp = base;
      for (unsigned j = 0, currentBit = 0; j < numBits; ++j) {
        if (unknowns[j]) {
          temp.setBitVal(j, (i >> currentBit) & 1);
          currentBit++;
        }
      }

      values.insert(temp);
    }

    return values;
  }

  std::set<APInt, APIntComparator>
  calculatePossibleValues(std::set<APInt, APIntComparator> v1,
                          std::set<APInt, APIntComparator> v2,
                          Instruction* inst) {
    std::set<APInt, APIntComparator> res;
    for (const auto& vv1 : v1) {
      printvalue2(vv1);
      for (const auto& vv2 : v2) {
        printvalue2(vv2);
        switch (inst->getOpcode()) {
        case Instruction::Add: {
          res.insert(vv1 + vv2);
          break;
        }
        case Instruction::Sub: {
          res.insert(vv1 - vv2);
          break;
        }
        case Instruction::Mul: {
          res.insert(vv1 * vv2);
          break;
        }
        case Instruction::LShr: {
          res.insert(vv1.lshr(vv2));
          break;
        }
        case Instruction::AShr: {
          res.insert(vv1.ashr(vv2));
          break;
        }
        case Instruction::Shl: {
          res.insert(vv1.shl(vv2));
          break;
        }
        case Instruction::UDiv: {
          if (!vv2.isZero()) {
            res.insert(vv1.udiv(vv2));
          }
          break;
        }
        case Instruction::URem: {
          res.insert(vv1.urem(vv2));
          break;
        }
        case Instruction::SDiv: {
          if (!vv2.isZero()) {
            res.insert(vv1.sdiv(vv2));
          }
          break;
        }
        case Instruction::SRem: {
          res.insert(vv1.srem(vv2));
          break;
        }
        case Instruction::And: {
          res.insert(vv1 & vv2);
          break;
        }
        case Instruction::Or: {
          res.insert(vv1 | vv2);
          break;
        }
        case Instruction::Xor: {
          res.insert(vv1 ^ vv2);
          break;
        }
        case Instruction::ICmp: {
          switch (cast<ICmpInst>(inst)->getPredicate()) {
          case llvm::CmpInst::ICMP_EQ: {
            res.insert(APInt(64, vv1.eq(vv2)));
            break;
          }
          case llvm::CmpInst::ICMP_NE: {
            res.insert(APInt(64, vv1.ne(vv2)));
            break;
          }
          case llvm::CmpInst::ICMP_SLE: {
            res.insert(APInt(64, vv1.sle(vv2)));
            break;
          }
          case llvm::CmpInst::ICMP_SLT: {
            res.insert(APInt(64, vv1.slt(vv2)));
            break;
          }
          case llvm::CmpInst::ICMP_ULE: {
            res.insert(APInt(64, vv1.ule(vv2)));
            break;
          }
          case llvm::CmpInst::ICMP_ULT: {
            res.insert(APInt(64, vv1.ult(vv2)));
            break;
          }
          case llvm::CmpInst::ICMP_SGE: {
            res.insert(APInt(64, vv1.sge(vv2)));
            break;
          }
          case llvm::CmpInst::ICMP_SGT: {
            res.insert(APInt(64, vv1.sgt(vv2)));
            break;
          }
          case llvm::CmpInst::ICMP_UGE: {
            res.insert(APInt(64, vv1.uge(vv2)));
            break;
          }
          case llvm::CmpInst::ICMP_UGT: {
            res.insert(APInt(64, vv1.ugt(vv2)));
            break;
          }
          default: {
            outs() << "\n : " << cast<ICmpInst>(inst)->getPredicate();
            outs().flush();
            llvm_unreachable_internal(
                "Unsupported operation in calculatePossibleValues ICMP.\n");
            break;
          }
          }
          break;
        }
        default:
          outs() << "\n : " << inst->getOpcode();
          outs().flush();
          llvm_unreachable_internal(
              "Unsupported operation in calculatePossibleValues.\n");
          break;
        }
      }
    }
    return res;
  }

  optional<KnownBits> KnownBitsRetardedOrWhat(KnownBits vv1, KnownBits vv2,
                                              Instruction* inst) {
    switch (inst->getOpcode()) {
    case Instruction::Add: {
      return KnownBits::computeForAddSub(1, 0, vv1, vv2);
      break;
    }
    case Instruction::Sub: {
      return KnownBits::computeForAddSub(0, 0, vv1, vv2);
      break;
    }
    case Instruction::Mul: {
      return KnownBits::mul(vv1, vv2);
      break;
    }
    case Instruction::LShr: {
      return KnownBits::lshr(vv1, vv2);
      break;
    }
    case Instruction::AShr: {
      return KnownBits::ashr(vv1, vv2);
      break;
    }
    case Instruction::Shl: {
      return KnownBits::shl(vv1, vv2);
      break;
    }
    case Instruction::UDiv: {
      if (!vv2.isZero()) {
        return (KnownBits::udiv(vv1, vv2));
      }
      break;
    }
    case Instruction::URem: {
      return KnownBits::urem(vv1, vv2);
      break;
    }
    case Instruction::SDiv: {
      if (!vv2.isZero()) {
        return KnownBits::sdiv(vv1, vv2);
      }
      break;
    }
    case Instruction::SRem: {
      return KnownBits::srem(vv1, vv2);
      break;
    }
    case Instruction::And: {
      return (vv1 & vv2);
      break;
    }
    case Instruction::Or: {
      return (vv1 | vv2);
      break;
    }
    case Instruction::Xor: {
      return (vv1 ^ vv2);
      break;
    }
    case Instruction::ICmp: {
      KnownBits kb(64);
      kb.setAllOnes();
      kb.setAllZero();
      kb.One ^= 1;
      kb.Zero ^= 1;
      switch (cast<ICmpInst>(inst)->getPredicate()) {
      case llvm::CmpInst::ICMP_EQ: {
        auto idk = KnownBits::eq(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      case llvm::CmpInst::ICMP_NE: {
        auto idk = KnownBits::eq(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      case llvm::CmpInst::ICMP_SLE: {
        auto idk = KnownBits::sle(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      case llvm::CmpInst::ICMP_SLT: {
        auto idk = KnownBits::slt(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      case llvm::CmpInst::ICMP_ULE: {
        auto idk = KnownBits::ule(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      case llvm::CmpInst::ICMP_ULT: {
        auto idk = KnownBits::ult(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      case llvm::CmpInst::ICMP_SGE: {
        auto idk = KnownBits::sge(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      case llvm::CmpInst::ICMP_SGT: {
        auto idk = KnownBits::sgt(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      case llvm::CmpInst::ICMP_UGE: {
        auto idk = KnownBits::uge(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      case llvm::CmpInst::ICMP_UGT: {
        auto idk = KnownBits::uge(vv1, vv2);
        if (idk.has_value()) {
          return KnownBits::makeConstant(APInt(64, idk.value()));
        }
        return kb;
        break;
      }
      default: {
        outs() << "\n : " << cast<ICmpInst>(inst)->getPredicate();
        outs().flush();
        llvm_unreachable_internal(
            "Unsupported operation in calculatePossibleValues ICMP.\n");
        break;
      }
      }
      break;
    }
    default:
      outs() << "\n : " << inst->getOpcode();
      outs().flush();
      llvm_unreachable_internal(
          "Unsupported operation in calculatePossibleValues.\n");
      break;
    }

    return nullopt;
  }

  set<APInt, APIntComparator> computePossibleValues(Value* V) {
    set<APInt, APIntComparator> res;
    printvalue(V);
    if (auto v_ci = dyn_cast<ConstantInt>(V)) {
      res.insert(v_ci->getValue());
      return res;
    }
    if (auto v_inst = dyn_cast<Instruction>(V)) {

      if (v_inst->getNumOperands() == 1)
        return computePossibleValues(v_inst->getOperand(0));

      if (v_inst->getOpcode() == Instruction::Select) {
        auto trueValue = v_inst->getOperand(1);
        auto falseValue = v_inst->getOperand(2);

        auto trueValues = computePossibleValues(trueValue);
        auto falseValues = computePossibleValues(falseValue);

        // Combine all possible values from both branches
        res.insert(trueValues.begin(), trueValues.end());
        res.insert(falseValues.begin(), falseValues.end());
        return res;
      }
      auto op1 = v_inst->getOperand(0);
      auto op2 = v_inst->getOperand(1);
      auto op1_knownbits = analyzeValueKnownBits(op1, v_inst);
      unsigned int op1_unknownbits_count = llvm::popcount(
          ~(op1_knownbits.One | op1_knownbits.Zero).getZExtValue());

      auto op2_knownbits = analyzeValueKnownBits(op2, v_inst);
      unsigned int op2_unknownbits_count = llvm::popcount(
          ~(op2_knownbits.One | op2_knownbits.Zero).getZExtValue());
      printvalue2(analyzeValueKnownBits(V, v_inst));
      auto v_knownbits = analyzeValueKnownBits(v_inst, v_inst);
      unsigned int res_unknownbits_count =
          llvm::popcount(~(v_knownbits.One | v_knownbits.Zero).getZExtValue());

      auto total_unk = ~((op1_knownbits.One | op1_knownbits.Zero) &
                         (op2_knownbits.One | op2_knownbits.Zero));

      unsigned int total_unknownbits_count =
          llvm::popcount(total_unk.getZExtValue());
      printvalue2(v_knownbits);
      printvalue2(op1_knownbits);
      printvalue2(op2_knownbits);
      printvalue2(res_unknownbits_count);
      printvalue2(op1_unknownbits_count);
      printvalue2(op2_unknownbits_count);
      printvalue2(total_unknownbits_count);

      if ((res_unknownbits_count >= total_unknownbits_count)) {
        auto v1 = computePossibleValues(op1);
        auto v2 = computePossibleValues(op2);

        printvalue(v_inst);
        printvalue2(v_knownbits);
        printvalue(op1);
        for (auto& vv1 : v1) {
          printvalue2(op1_knownbits);
          printvalue2(vv1);
        }
        printvalue(op2);
        for (auto& vv2 : v2) {
          printvalue2(op2_knownbits);
          printvalue2(vv2);
        }
        return calculatePossibleValues(v1, v2, v_inst);
      }
      return getPossibleValues(v_knownbits, res_unknownbits_count);
    }
    return res;
  }

  Value* solveLoad(LoadInst* load) {
    printvalue(load);

    // replace this
    auto LoadMemLoc = MemoryLocation::get(load);

    const Value* loadPtr = LoadMemLoc.Ptr;
    LocationSize loadsize = LoadMemLoc.Size;

    auto cloadsize = loadsize.getValue();

    auto loadPtrGEP = cast<GetElementPtrInst>(loadPtr);

    auto loadPointer = loadPtrGEP->getPointerOperand();
    auto loadOffset = loadPtrGEP->getOperand(1);
    printvalue(loadOffset);

    // if we know all the stores, we can use our buffer
    // however, if we dont know all the stores
    // we have to if check each store overlaps with our load
    // specifically for indirect stores
    IRBuilder<> builder(load);
    if (isa<ConstantInt>(loadOffset)) {
      auto loadOffsetCI = cast<ConstantInt>(loadOffset);

      auto loadOffsetCIval = loadOffsetCI->getZExtValue();

      auto valueExtractedFromVirtualStack = VirtualStack.retrieveCombinedValue(
          builder, loadOffsetCIval, cloadsize, load);
      if (valueExtractedFromVirtualStack) {
        return valueExtractedFromVirtualStack;
      }
    } else {
      // Get possible values from loadOffset
      auto x = computePossibleValues(loadOffset);
      llvm::Value* selectedValue = nullptr;
      for (auto delirdimgaliba : x) {
        printvalue2(delirdimgaliba);
      }

      for (auto xx : x) {

        auto isPaged = isMemPaged(xx.getZExtValue());
        if (!isPaged)
          continue;
        printvalue2(xx);
        auto possible_values_from_mem = VirtualStack.retrieveCombinedValue(
            builder, xx.getZExtValue(), cloadsize, load);
        printvalue2((uint64_t)cloadsize);
        printvalue(possible_values_from_mem);

        if (selectedValue == nullptr) {
          selectedValue = possible_values_from_mem;
        } else {

          llvm::Value* comparison = createICMPFolder(
              builder, CmpInst::ICMP_EQ, loadOffset,
              llvm::ConstantInt::get(loadOffset->getType(), xx));
          printvalue(comparison);
          selectedValue =
              createSelectFolder(builder, comparison, possible_values_from_mem,
                                 selectedValue, "conditional-mem-load");
        }
      }
      return selectedValue;
    }

    // create a new vector with only leave what we care about
    vector<Instruction*> clearedMemInfos;

    clearedMemInfos = memInfos;
    removeDuplicateOffsets(clearedMemInfos);

    Value* retval = nullptr;

    for (auto inst : clearedMemInfos) {

      // we are only interested in previous instructions

      // replace it with something more efficent
      // auto MemLoc = MemoryLocation::get(inst);

      StoreInst* storeInst = cast<StoreInst>(inst);
      auto memLocationValue = storeInst->getPointerOperand();

      auto memLocationGEP = cast<GetElementPtrInst>(memLocationValue);

      auto pointer = memLocationGEP->getOperand(0);
      auto offset = memLocationGEP->getOperand(1);

      if (pointer != loadPointer)
        break;

      // find a way to compare with unk values, we are also interested
      // when offset in unk ( should be a rare case )
      if (!isa<ConstantInt>(offset) || !isa<ConstantInt>(loadOffset))
        continue;

      uint64_t memOffsetValue = cast<ConstantInt>(offset)->getZExtValue();
      uint64_t loadOffsetValue = cast<ConstantInt>(loadOffset)->getZExtValue();

      uint64_t diff = memOffsetValue - loadOffsetValue;

      // this is bytesize, not bitsize
      uint64_t storeBitSize =
          storeInst->getValueOperand()->getType()->getIntegerBitWidth() / 8;

      if (overlaps(loadOffsetValue, cloadsize, memOffsetValue, storeBitSize)) {

        printvalue2(diff) printvalue2(memOffsetValue);
        printvalue2(loadOffsetValue) printvalue2(storeBitSize);

        auto storedInst = inst->getOperand(0);
        if (!retval)
          retval = ConstantInt::get(load->getType(), 0);

        Value* mask = ConstantInt::get(
            storedInst->getType(),
            createmask(loadOffsetValue, loadOffsetValue + cloadsize,
                       memOffsetValue, memOffsetValue + storeBitSize));

        printvalue(mask);

        IRBuilder<> builder(load);
        // we dont have to calculate knownbits if its a constant
        auto maskedinst = createAndFolder(builder, storedInst, mask,
                                          inst->getName() + ".maskedinst");

        printvalue(storedInst);
        printvalue(mask);
        printvalue(maskedinst);
        if (maskedinst->getType()->getScalarSizeInBits() <
            retval->getType()->getScalarSizeInBits())
          maskedinst = builder.CreateZExt(maskedinst, retval->getType());

        if (mask->getType()->getScalarSizeInBits() <
            retval->getType()->getScalarSizeInBits())
          mask = builder.CreateZExt(mask, retval->getType());

        printvalue(maskedinst);
        printvalue2(diff);
        // move the mask?
        if (diff > 0) {
          maskedinst = createShlFolder(builder, maskedinst, (diff) * 8);
          mask = createShlFolder(builder, mask, (diff) * 8);
        } else if (diff < 0) {
          maskedinst =
              createLShrFolder(builder, maskedinst, -(diff) * 8, "clevername");
          mask = createLShrFolder(builder, mask, -(diff) * 8, "stupidname");
        }
        // maskedinst = maskedinst
        // maskedinst = 0x4433221100000000
        printvalue(maskedinst);
        maskedinst = builder.CreateZExtOrTrunc(maskedinst, retval->getType());
        printvalue(maskedinst);

        printvalue(mask);

        // clear mask from retval so we can merge
        // this will be a NOT operation for sure

        auto reverseMask = builder.CreateNot(mask);

        printvalue(reverseMask);

        auto cleared_retval =
            createAndFolder(builder, retval,
                            builder.CreateTrunc(reverseMask, retval->getType()),
                            retval->getName() + ".cleared");
        // cleared_retval = 0 & 0; clear retval
        // cleared_retval = retval & 0xff_ff_ff_ff_00_00_00_00

        retval = createOrFolder(builder, cleared_retval, maskedinst,
                                cleared_retval->getName() + ".merged");
        // retval = builder.CreateTrunc(retval, load->getType());
        printvalue(cleared_retval);
        printvalue(maskedinst);
        // retval = cleared_retval | maskedinst =|= 0 |
        // 0x1122334455667788 retval = cleared_retval | maskedinst =|=
        // 0x55667788 | 0x4433221100000000

        if (retval)
          if (retval->getType()->getScalarSizeInBits() >
              load->getType()->getScalarSizeInBits())
            retval = builder.CreateTrunc(retval, load->getType());

        printvalue(inst);
        auto retvalload = retval;
        printvalue(cleared_retval);
        printvalue(retvalload);
        debugging::doIfDebug([&]() { cout << "-------------------\n"; });
      }
    }
    return retval;
  }
}; // namespace GEPStoreTracker

// some stuff about memory
// partial load example
//
// %m1 = getelementptr i8, %memory, i64 0
// %m2 = getelementptr i8, %memory, i64 4
// %m3 = getelementptr i8, %memory, i64 8
// store i64 0x11_22_33_44_55_66_77_88, ptr %m1 => [0] 88 77 66 55 [4] 44 33
// 22 11 [8] store i64 0xAA_BB_CC_DD_EE_FF_AB_AC, ptr %m3 => [0] 88 77 66 55
// [4] 44 33 22 11 [8] AC AB FF EE [12] DD CC BB AA [16] %x = load i64, ptr
// %m2 => [0] 88 77 66 55 [4] 44 33 22 11 [8] AC AB FF EE [12] DD CC BB AA
// [16] now: %x = 44 33 22 11 AC AB FF EE => 0xEE_FF_AB_AC_11_22_33_44 %p1 =
// 0x11_22_33_44_55_66_77_88 & 0xFF_FF_FF_FF_00_00_00_00 %p2 =
// 0xAA_BB_CC_DD_EE_FF_AB_AC & 0x00_00_00_00_FF_FF_FF_FF %p3 = 0 %p1.shift =
// %p1
// >> 4(diff)*8 %p2.shift = %p2 << 4(diff)*8 %p4 = %p1.shift | %p2.shift
//
// overwriting example
//
//
//
// %m1 = getelementptr i8, %memory, i64 0
// %m2 = getelementptr i8, %memory, i64 2
// %m3 = getelementptr i8, %memory, i64 8
// store i64 0x11_22_33_44_55_66_77_88, ptr %m1 => [0] 88 77 [2] 66 55 [4] 44
// 33 22 11 [8] store i64 0xAA_BB_CC_DD_EE_FF_AB_AC, ptr %m2 => [0] 88 77 [2]
// AC AB [4] FF EE DD CC [8] BB AA [10] %x = load i64, ptr %m1 => [0] 88 77
// [2] AC AB [4] FF EE DD CC [8] BB AA [10] now: %x = 88 77 AC AB FF EE DD CC
// => 0xCC_DD_EE_FF_AB_AC_11_22 %p1 = 0x11_22_33_44_55_66_77_88 & -1 %p2 =
// 0xAA_BB_CC_DD_EE_FF_AB_AC & 0x00_00_FF_FF_FF_FF_FF_FF %p2.shifted = %p2 <<
// 2*8 %mask.shifted = 0x00_00_FF_FF_FF_FF_FF_FF << 2*8 =>
// 0xFF_FF_FF_FF_FF_FF_00_00 %reverse.mask.shifted = 0xFF_FF %p1.masked = %p1
// & %reverse.mask.shifted %retval = %p2.shifted | %p1.masked
//
// overwriting example WITH DIFFERENT TYPES
//
//
//
// %m1 = getelementptr i8, %memory, i64 0
// %m2 = getelementptr i8, %memory, i64 3
// %m3 = getelementptr i8, %memory, i64 8
// store i64 0x11_22_33_44_55_66_77_88, ptr %m1 => [0] 88 77 66 [3] 55 44 33
// 22 [7] 11 [8] store i32 0xAA_BB_CC_DD, ptr %m2             => [0] 88 77 66
// [3] DD CC BB AA [7] 11 [8] %x = load i64, ptr %m1                       =>
// [0] 88 77 66 [3] DD CC BB AA [7] 11 [8] now: %x=[0] 88 77 66 [3] DD CC BB
// AA [7] 11 [8] => 0x11_AA_BB_CC_DD_66_77_88 %p1 = 0x11_22_33_44_55_66_77_88
// & -1 %p2 = 0xAA_BB_CC_DD & 0xFF_FF_FF_FF %p2.shifted = %p2 << 1*8 =>
// 0xAA_BB_CC_DD << 8 => 0x_AA_BB_CC_DD_00 %mask.shifted = 0xFF_FF_FF_FF <<
// 1*8
// => 0x00_00_00_FF_FF_FF_FF_00 %reverse.mask.shifted =
// 0xFF_FF_FF_00_00_00_00_FF %p1.masked = %p1 & %reverse.mask.shifted =>
// 0x11_22_33_44_55_66_77_88 & 0xFF_FF_FF_00_00_00_00_FF =>
// 0x11_22_33_00_00_00_00_88 %retval = %p2.shifted | %p1.masked       =>
// 0x11_22_33_00_00_00_00_88 | 0x00_00_00_AA_BB_CC_DD_00 =>
// 0x11_22_33_AA_BB_CC_DD_88
//
// PARTIAL overwriting example WITH DIFFERENT TYPES v1
//
//
//
// %m1 = getelementptr i8, %memory, i64 0
// %m2 = getelementptr i8, %memory, i64 6
// %m3 = getelementptr i8, %memory, i64 8
// store i64 0x11_22_33_44_55_66_77_88, ptr %m1 => [0] 88 77 66 [3] 55 44 33
// [6] 22 11 [8] store i32 0xAA_BB_CC_DD, ptr %m2             => [0] 88 77 66
// [3] 55 44 33 [6] DD CC [8] BB AA [10] %x = load i64, ptr %m1 => [0] 88 77
// 66 [3] 55 44 33 [6] DD CC [8] BB AA [10] now: %x=[0] 88 77 66 [3] 55 44 33
// [6] DD CC [8] => 0xCC_DD_33_44_55_66_77_88 %p1 = 0x11_22_33_44_55_66_77_88
// & -1 %p2 = 0xAA_BB_CC_DD & 0x00_00_FF_FF %p2.shifted = %p2 << 6*8 =>
// 0xCC_DD << 48 => 0xCC_DD_00_00_00_00_00_00 %mask.shifted = 0xFF_FF_FF_FF <<
// 6*8     => 0xFF_FF_00_00_00_00_00_00 %reverse.mask.shifted =
// 0x00_00_FF_FF_FF_FF_FF_FF %p1.masked = %p1 & %reverse.mask.shifted =>
// 0x11_22_33_44_55_66_77_88 & 0x00_00_FF_FF_FF_FF_FF_FF =>
// 0x00_00_33_44_55_66_77_88 %retval = %p2.shifted | %p1.masked       =>
// 0x00_00_33_44_55_66_77_88 | 0xCC_DD_00_00_00_00_00_00 =>
// 0xCC_DD_33_44_55_66_77_88
//
//
// PARTIAL overwriting example WITH DIFFERENT TYPES v2
//
//
//
// %m1 = getelementptr i8, %memory, i64 8
// %m2 = getelementptr i8, %memory, i64 7
// %m3 = getelementptr i8, %memory, i64 16
// store i64 0x11_22_33_44_55_66_77_88, ptr %m1 => [7] ?? [8] 88 77 66 [11] 55
// 44 33 22 11 [16] store i32 0xAA_BB_CC_DD, ptr %m2             => [7] DD [8]
// CC BB AA [11] 55 44 33 22 11 [16] %x = load i64, ptr %m1 => [7] DD [8] CC
// BB AA [11] 55 44 33 22 11 [16] now: %x=[7] DD [8] CC BB AA [11] 55 44 33 22
// 11 [16] => 0xCC_DD_33_44_55_66_77_88 %p1 = 0x11_22_33_44_55_66_77_88 & -1
// %p2 = 0xAA_BB_CC_DD & 0xFF_FF_FF_00 (0xFF ^ -1) %p2.shifted = %p2 << 1*8 =>
// 0xAA_BB_CC_00 >> 8 => 0xAA_BB_CC => 0x00_00_00_00_00_AA_BB_CC %mask.shifted
// = 0xFF_FF_FF_00 >> 1*8     => 0xFF_FF_FF %reverse.mask.shifted =
// 0xFF_FF_FF_FF_FF_00_00_00 %p1.masked = %p1 & %reverse.mask.shifted =>
// 0x11_22_33_44_55_66_77_88 & 0xFF_FF_FF_FF_FF_00_00_00 =>
// 0x11_22_33_44_55_00_00_00 %retval = %p2.shifted | %p1.masked       =>
// 0x11_22_33_44_55_00_00_00 | 0xAA_BB_CC                =>
// 0x11_22_33_44_55_AA_BB_CC
//
//
// creating masks:
// orgload = 0<->8
// currentstore = 4<->8
// size = 32bits
// mask will be:
// 0xFF_FF_FF_FF_00_00_00_00
//
// orgload = 0<->8
// currentstore = 3<->7
// size = 32bits
// mask will be:
// 0x00_FF_FF_FF_FF_00_00_00
//
// orgload = 0<->8
// currentstore = 6<->10
// size = 32bits
// mask will be:
// 0xFF_FF_00_00_00_00_00_00
//
// orgload = 10<->18
// currentstore = 8<->16
// size = 32bits
// mask will be:
// 0x00_00_00_00_00_00_FF_FF
//
// mask generation:
// a1 = loadStart
// a2 = loadEnd
// b1 = storeStart
// b2 = storeEnd
//  a1, a2, b1, b2
// (assuming they overlap)
// [6 = b1] [7] [8 = a1] [9] [10 = b2] [11] [12 = a2]
//    -      -      +     +     -       /       /
// normal mask for b =  0xFF_FF_00_00
// clear  mask for a = ~0x00_00_FF_FF
//
// shift size = 2 (a1-b1, since its +, shift to right)
//
//
// [8 = a1] [9] [10] [11 = b1] [12 = a2] [13] [14 = b2]
//    -      -    -      +        /        /      /
//
// normal mask for b =  0x00_00_00_FF (lowest byte gets saved)
// clear  mask for a = ~0xFF_00_00_00 (only highest byte gets cleared)
//
// shift size = -3 (a1-b1, since its -, shift to left)
//
// first iteration in loop
// store = getstore(currentStore)
// createMask( diff )
// shiftStore  = Store1 << diff
// shiftedmask = mask << diff
// reverseMask = ~shiftedmask
// retvalCleared = retval & reverseMask
// retval = retvalCleared | shiftStore
// second iteration in loop
//
// store = getstore(currentStore)
// createMask( diff )
// shiftStore  = Store1 << diff
// shiftedmask = mask << diff
// reverseMask = ~shiftedmask
// retvalCleared = retval & reverseMask
// retval = retvalCleared | shiftStore
//
//
//