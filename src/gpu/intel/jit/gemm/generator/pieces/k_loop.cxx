/*******************************************************************************
* Copyright 2019-2025 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/


#include "compute_utils.hpp"
#include "gemmstone/generator.hpp"
#include "hw_utils.hpp"
#include "kernel_queries.hpp"
#include "layout_utils.hpp"
#include "loop_sequencer.hpp"
#include "remask.hpp"
#include "state_utils.hpp"
#include "quantization.hpp"

GEMMSTONE_NAMESPACE_START

using namespace ngen;
using std::vector;


// Create 1-segment inner loop for a GEMM-like kernel.
template <HW hw>
bool Generator<hw>::kLoopSingle(KLoop type, const GEMMProblem &problem, GEMMStrategy &strategy, GEMMState &state)
{
    bool ok = kLoopSetup(problem, strategy, state);
    if (ok) {
        kLoop(type, problem, strategy, state);
        kLoopTeardown(problem, strategy, state);
    }
    return ok;
}

// Create one step of a sequence of inner loops for a GEMM-like kernel.
template <HW hw>
void Generator<hw>::kLoop(KLoop type, const GEMMProblem &problem, GEMMStrategy &strategy, GEMMState &state)
{
    auto Ta = problem.Ta, Tb = problem.Tb, Tc = problem.Tc;
    auto Ta_ext = problem.Ta_ext, Tb_ext = problem.Tb_ext;
    auto Ta_load = state.Ta_load, Tb_load = state.Tb_load;

    bool cLoadAhead = strategy.cLoadAhead;
    auto opCountMain = outerProductCount(hw, problem, strategy);
    auto minOPCount = minOuterProductCount(hw, problem, strategy);
    auto opCountRem = minOPCount;

    auto A_copies = strategy.A_copies;
    auto B_copies = strategy.B_copies;
    auto slmCopies = strategy.slmCopies;
    auto slmBuffers = strategy.slmBuffers;
    auto ka_loadMain = strategy.ka_load, ka_loadRem = state.ka_loadRem;
    auto kb_loadMain = strategy.kb_load, kb_loadRem = state.kb_loadRem;
    auto ka_repackMain = state.ka_repack, ka_repackRem = state.ka_repackRem;
    auto ka_pfStride = strategy.ka_pfStride;
    auto kb_pfStride = strategy.kb_pfStride;
    auto kInterleaveChunk = strategy.kInterleaveChunk;
    bool slmA = strategy.slmA;
    bool slmB = strategy.slmB;
    bool slmASums = state.slmASums;
    bool slmBSums = state.slmBSums;
    auto unrollM = strategy.unroll[LoopM];
    auto unrollN = strategy.unroll[LoopN];
    auto unrollK = strategy.unroll[LoopK];
    auto unrollKSLM = strategy.unrollKSLM;
    bool calcASums = problem.needsASums();
    bool calcBSums = problem.needsBSums();
    bool readA = true, readB = true;

    bool dequantizeA = problem.earlyDequantizeA();
    bool dequantizeB = problem.earlyDequantizeB();
    bool slmDequantizeA = dequantizeA && slmA;
    bool slmDequantizeB = dequantizeB && slmB;
    dequantizeA &= !slmDequantizeA;
    dequantizeB &= !slmDequantizeB;

    bool ao2D = (problem.aoPtrDims == 2), as2D = problem.aScale2D();
    bool bo2D = (problem.boPtrDims == 2), bs2D = problem.bScale2D();
    bool ao2DLate = ao2D && problem.needsBGroupSums();
    bool bo2DLate = bo2D && problem.needsAGroupSums();
    bool as2DLate = as2D && state.lateScale2DA;
    bool bs2DLate = bs2D && state.lateScale2DB;
    bool ag2DLate = problem.needsAGroupSums();
    bool bg2DLate = problem.needsBGroupSums();
    ao2D &= !ao2DLate;
    bo2D &= !bo2DLate;
    as2D &= !as2DLate;
    bs2D &= !bs2DLate;
    bool dequantize2DA = ao2D || as2D;
    bool dequantize2DB = bo2D || bs2D;
    bool dequantize2DALate = ao2DLate || as2DLate || ag2DLate;
    bool dequantize2DBLate = bo2DLate || bs2DLate || bg2DLate;
    bool slmDequantize2DA = dequantize2DA && slmA;
    bool slmDequantize2DB = dequantize2DB && slmB;
    dequantize2DA &= !slmDequantize2DA;
    dequantize2DB &= !slmDequantize2DB;
    int aqGroupK = problem.aqGroupK;
    int bqGroupK = problem.bqGroupK;
    int kaq_load = aqGroupK * state.kaq;
    int kbq_load = bqGroupK * state.kbq;
    int kaq_loadLate = aqGroupK * state.kaqLate;
    int kbq_loadLate = bqGroupK * state.kbqLate;

    if (kInterleaveChunk) {
        kaq_load = std::min(kaq_load, kInterleaveChunk);
        kbq_load = std::min(kbq_load, kInterleaveChunk);
        kaq_loadLate = std::min(kaq_loadLate, kInterleaveChunk);
        kbq_loadLate = std::min(kbq_loadLate, kInterleaveChunk);
    }

    bool Ai_incrementalRem = state.Ai_incrementalRem;
    bool Bi_incrementalRem = state.Bi_incrementalRem;
    bool Ai_remIncrCopy = state.Ai_remIncrCopy;
    bool Bi_remIncrCopy = state.Bi_remIncrCopy;

    bool &remActiveA = state.remActiveA, &remActiveB = state.remActiveB;
    bool &remActiveSLM = state.remActiveSLM;
    auto &kMasksAi = state.kMasksAi, &kMasksBi = state.kMasksBi;
    bool &slmRemaskA = state.slmRemaskA, &slmRemaskB = state.slmRemaskB;
    bool lateKLoopCheck = state.lateKLoopCheck;

    auto &kSLMA = state.kSLMA, &kSLMB = state.kSLMB;
    auto &kSLMStorage = state.kSLMStorage;

    bool needBarrier = (slmA || slmB || strategy.barrierFreq > 0);
    bool nbM = (slmA || strategy.barrierFreq) && strategy.namedBarriers[LoopM];
    bool nbN = (slmB || strategy.barrierFreq) && strategy.namedBarriers[LoopN];
    bool needUnnamedBarrier = (slmA && !nbM) || (slmB && !nbN) || (strategy.barrierFreq && !nbM && !nbN);

    bool noFinalBarrier = state.kNoBarrierStart.isValid() || state.kNoBarrierEnd.isValid();
    bool barrierTask = (strategy.barrierFreq > 0) && (strategy.barrierFreq <= unrollK) && !noFinalBarrier;
    bool barrierSubloop = (strategy.barrierFreq > 0) && !barrierTask;

    bool needXPReset = false;

    int curPhase;
    int &lastThresh = state.lastThresh;

    lastThresh = 0;

    bool loadBFirst = strategy.loadBFirst && readA && readB;

    // Get r0 information where needed.
    GRF r0_info;
    if (needBarrier) {
        if (state.r0_info.isARF()) stub();
        r0_info = GRF{state.r0_info.getBase()};
    }

    // Unified barrier and SLM fence handling for k loop.
    auto &modBarrierFence = state.modBarrierFence;
    auto &barrierHeader = state.barrierHeader;
    auto &barrierReady = state.barrierReady;

    auto getFenceTemp = [&]() {
        auto temp = state.ra.try_alloc();
        if (temp.isValid())
            return temp;
        if (barrierHeader.isValid()) {
            barrierReady = false;
            return barrierHeader;
        }
        throw ngen::out_of_registers_exception();
    };

    auto releaseFenceTemp = [&](GRF temp) {
        if (temp.isValid() && temp != barrierHeader)
            state.ra.release(temp);
    };

    GRF slmFenceTemp;
    auto slmFenceIssue = [&]() {
        slmFenceTemp = getFenceTemp();
        slmfence(modBarrierFence[0], slmFenceTemp, r0_info);
        releaseFenceTemp(slmFenceTemp);
    };

    if (slmA && slmB && nbM != nbN) stub();

    auto kLoopBarrier = [&](bool withSLMFence, KBarrierType type) {
        if (withSLMFence && type == KBarrierType::Wait) {
            auto temp = getFenceTemp();
            slmfence(modBarrierFence[0], temp, r0_info);
            fencewait();
            releaseFenceTemp(temp);
        }

        if (!nbM && !nbN) {
            if (type != KBarrierType::Wait) {
                kLoopAllocBarrierHeader(state);
                auto temp = getFenceTemp();
                if (withSLMFence) {
                    slmfence(modBarrierFence[0], temp, r0_info);
                    fencewait();
                }
                auto header = kLoopGetBarrierHeader(strategy, state);
                barriermsg(modBarrierFence[0], header);
                releaseFenceTemp(temp);
            }
            if (type != KBarrierType::Signal)
                barrierwait();
        } else {
            if (type != KBarrierType::Wait) {
                if (withSLMFence) {
                    auto temp = getFenceTemp();
                    slmfence(modBarrierFence[0], temp, r0_info);
                    fencewait();
                    releaseFenceTemp(temp);
                }
                if (nbM) barriermsg(modBarrierFence[0],           state.barrierHeaderM);
                if (nbN) barriermsg(modBarrierFence[nbM ? 1 : 0], state.barrierHeaderN);
            }
            if (type != KBarrierType::Signal) {
                if (nbM) sync.bar(state.barrierM);
                if (nbN) sync.bar(state.barrierN);
            }
        }
    };

    bool mustActivateRemainder = state.A_lateKRem || state.B_lateKRem;
    bool mustActivateSLMRemainder = state.Ai_lateKRem || state.Bi_lateKRem;

    auto resetKSLM = [&]() {
        state.ra.safeRelease(kSLMStorage);
        kSLMA = kSLMB = invalid;
    };

    // Get state.K, the loop counter.
    // The caller may initialize state.K, in case its value on entry is the loop count.
    // Otherwise, it is initialized from state.k.
    auto kInput = state.k;
    bool matchBarriers = (strategy.kParallelLocal && needUnnamedBarrier);
    bool saveK = state.isNested || matchBarriers || strategy.fusePostOps
              || (problem.aOffset != ABOffset::None && problem.bOffset != ABOffset::None);
    bool incomingK = state.K.isValid();

    if (!incomingK)
        state.K = saveK ? state.ra.alloc_sub<int32_t>() : kInput;

    if (saveK && !incomingK)
        mov(1, state.K, kInput);

    if (state.firstKLoopSegment) {
        // Zero out A/B sums if needed.
        if (calcASums) zeroMatrix(state.As_regs, strategy);
        if (calcBSums) zeroMatrix(state.Bs_regs, strategy);

        // Zero out C, if not loading ahead of time.
        if (!cLoadAhead && !(strategy.registerOutput() && problem.beta1())) {
            for (int i = 0; i < state.C_accCount; i += 2)
                mov<uint32_t>(2 * elementsPerGRF<uint32_t>(hw), AccumulatorRegister(i), uint16_t(0));

            for (int buf = 0; buf < state.C_buffers; buf++)
                zeroMatrix(state.C_regs[buf], strategy);
        }

        if (strategy.fuseBeta && !strategy.altFusedBeta)
            gemmFusedBetaNotifyCompletion(problem, strategy, state);
    }

    LoopSequencer ls;
    using namespace loop_sequencer;

    int slmBufferLA = 0;
    switch (slmBuffers) {
        case 0:
        case 1: slmBufferLA = 0; break;
        case 2:
        case 3: slmBufferLA = 1; break;
        case 4: slmBufferLA = 2; break;
        default: stub();
    }

    int lookaheadALoad    = ka_loadMain * (A_copies - 1);
    int lookaheadBLoad    = kb_loadMain * (B_copies - 1);
    int lookaheadALoadRem = ka_loadRem  * (A_copies - 1);
    int lookaheadBLoadRem = kb_loadRem  * (B_copies - 1);
    int lookaheadSLMLoad  = unrollKSLM  * (slmCopies - 1) + unrollKSLM - 1;
    int lookaheadSLMStore = unrollKSLM  * slmBufferLA + 1;

    if (slmA && slmB) {
        if (lookaheadALoad != lookaheadBLoad) stub();
        if (lookaheadALoadRem != lookaheadBLoadRem) stub();
        if (ka_loadMain != kb_loadMain && lookaheadALoad != lookaheadALoadRem) stub();
    }

    int lookaheadSLMReload    = slmA ? lookaheadALoad    : lookaheadBLoad;
    int lookaheadSLMReloadRem = slmA ? lookaheadALoadRem : lookaheadBLoadRem;
    int durationSLMMainLoad   = std::max(slmA * ka_loadMain, slmB * kb_loadMain);

    int lookaheadAGlobalLoad = lookaheadALoad;
    int lookaheadBGlobalLoad = lookaheadBLoad;
    if (slmA) lookaheadAGlobalLoad = lookaheadSLMLoad + lookaheadSLMStore + lookaheadSLMReload;
    if (slmB) lookaheadBGlobalLoad = lookaheadSLMLoad + lookaheadSLMStore + lookaheadSLMReload;

    auto A_remActive = [&](Iteration h) { return (h.remaining() < ka_loadMain - (h % ka_loadMain)); };
    auto B_remActive = [&](Iteration h) { return (h.remaining() < kb_loadMain - (h % kb_loadMain)); };
    auto slmRemActive = [&](Iteration h) { return (h.remaining() < unrollKSLM - (h % unrollKSLM)); };
    auto opRemActive = [&](Iteration h) { return (h.remaining() < opCountMain - (h % opCountMain)); };
    auto repackA = [&](Iteration h) { return A_remActive(h) ? state.repackARem : state.repackA; };
    auto repackB = [&](Iteration h) { return B_remActive(h) ? state.repackBRem : state.repackB; };
    auto ka_load = [&](Iteration h) { return A_remActive(h) ? ka_loadRem : ka_loadMain; };
    auto kb_load = [&](Iteration h) { return B_remActive(h) ? kb_loadRem : kb_loadMain; };
    auto ka_repack = [&](Iteration h) { return !state.repackA ? ka_load(h) : A_remActive(h) ? ka_repackRem : ka_repackMain; };
    auto A_copy = [&](Iteration h) { return (h / ka_load(h)) % A_copies; };
    auto B_copy = [&](Iteration h) { return (h / kb_load(h)) % B_copies; };
    auto A_regs = [&](Iteration h) -> GRFMultirange& { return state.A_regs[A_copy(h)]; };
    auto B_regs = [&](Iteration h) -> GRFMultirange& { return state.B_regs[B_copy(h)]; };
    auto A_layout = [&](Iteration h) -> RegisterLayout& { return A_remActive(h) ? state.A_layoutRem : state.A_layout; };
    auto B_layout = [&](Iteration h) -> RegisterLayout& { return B_remActive(h) ? state.B_layoutRem : state.B_layout; };
    auto Ar_regs = [&](Iteration h) -> GRFMultirange& { return repackA(h) ? state.Ar_regs : A_regs(h); };
    auto Br_regs = [&](Iteration h) -> GRFMultirange& { return repackB(h) ? state.Br_regs : B_regs(h); };
    auto Ar_layout = [&](Iteration h) -> RegisterLayout& { return repackA(h) ? state.Ar_layout : A_layout(h); };
    auto Br_layout = [&](Iteration h) -> RegisterLayout& { return repackB(h) ? state.Br_layout : B_layout(h); };
    auto slmCopy = [&](Iteration h) { return (h / unrollKSLM) % slmCopies; };
    auto slmBuffer = [&](Iteration h) { return (h / unrollKSLM) % slmBuffers; };
    auto Ai_layout = [&](Iteration h) -> RegisterLayout& { return slmRemActive(h) ? state.Ai_layoutRem : state.Ai_layout; };
    auto Bi_layout = [&](Iteration h) -> RegisterLayout& { return slmRemActive(h) ? state.Bi_layoutRem : state.Bi_layout; };
    auto Ai_addrs = [&](Iteration h) -> vector<GRFRange>& { return slmRemActive(h) ? state.Ai_addrsRem : state.Ai_addrs; };
    auto Bi_addrs = [&](Iteration h) -> vector<GRFRange>& { return slmRemActive(h) ? state.Bi_addrsRem : state.Bi_addrs; };
    auto Ai_allRegs = [&](Iteration h) -> vector<GRFMultirange>& { return slmRemActive(h) ? state.Ai_regsRem : state.Ai_regs; };
    auto Bi_allRegs = [&](Iteration h) -> vector<GRFMultirange>& { return slmRemActive(h) ? state.Bi_regsRem : state.Bi_regs; };
    auto Ai_regs = [&](Iteration h) -> GRFMultirange& { return Ai_allRegs(h)[slmCopy(h)]; };
    auto Bi_regs = [&](Iteration h) -> GRFMultirange& { return Bi_allRegs(h)[slmCopy(h)]; };
    auto Ao_regs = [&](Iteration h) -> GRFMultirange& { return slmRemActive(h) ? state.Ao_regsRem : state.Ao_regs; };
    auto Bo_regs = [&](Iteration h) -> GRFMultirange& { return slmRemActive(h) ? state.Bo_regsRem : state.Bo_regs; };
    auto effAo_regs = [&](Iteration h) -> GRFMultirange& { return Ao_regs(h).empty() ? Ai_regs(h) : Ao_regs(h); };
    auto effBo_regs = [&](Iteration h) -> GRFMultirange& { return Bo_regs(h).empty() ? Bi_regs(h) : Bo_regs(h); };
    auto aioShare = [&](Iteration h) { return slmRemActive(h) ? state.aioShareRem : state.aioShare; };
    auto bioShare = [&](Iteration h) { return slmRemActive(h) ? state.bioShareRem : state.bioShare; };
    auto opCount = [&](Iteration h) { return opRemActive(h) ? opCountRem : opCountMain; };
    auto nothing = [&](Iteration h) {};
    auto never = [&](Iteration h) { return false; };

    auto kInc = [&](Iteration h, int k_inc, int group = 1) {
        if (kInterleaveChunk) {
            k_inc *= group;
            if (k_inc > kInterleaveChunk)
                k_inc = kInterleaveChunk * strategy.wg[LoopK];
            else if ((h % kInterleaveChunk + k_inc) >= kInterleaveChunk)
                k_inc += kInterleaveChunk * (strategy.wg[LoopK] - 1);
            k_inc /= group;
        }
        return k_inc;
    };

    // Dummy task to extend k unroll if needed.
    ls.schedule(every(unrollK) | checkOptional(), nothing);

    // A prefetch.
    int aPFDuration = strategy.cooperativePF ? ka_pfStride : strategy.ka_prefetch;
    if (isBlock2D(strategy.A_prefetch.accessType))
        aPFDuration = 1;        /* allow block 2D prefetches in k remainder */

    auto reqPFA = every(ka_pfStride)
                | duration(aPFDuration)
                | lookahead(strategy.prefetchA + lookaheadAGlobalLoad);

    if (strategy.prefetchA && readA) {
        ls.schedule(reqPFA, [&](Iteration h) {
            gemmALoad(state.Ap_regs, state.Ap_layout, state.Ap_addrs, problem, strategy, state);
        });
    }

    // B prefetch.
    int bPFDuration = strategy.cooperativePF ? kb_pfStride : strategy.kb_prefetch;
    if (isBlock2D(strategy.B_prefetch.accessType))
        bPFDuration = 1;

    auto reqPFB = every(kb_pfStride)
                | duration(bPFDuration)
                | lookahead(strategy.prefetchB + lookaheadBGlobalLoad);

    if (strategy.prefetchB && readB) {
        ls.schedule(reqPFB, [&](Iteration h) {
            gemmBLoad(state.Bp_regs, state.Bp_layout, state.Bp_addrs, problem, strategy, state);
        });
    }

    // A/B L3 prefetch.
    gemmScheduleL3Prefetches(&ls, problem, strategy, state);

    if (slmDequantize2DA && slmDequantize2DB && kaq_load != kbq_load) stub();
    int slmKQLoad = slmDequantize2DA ? kaq_load : kbq_load;
    bool slmDequantize2D = (slmDequantize2DA || slmDequantize2DB);
    if (slmDequantize2D)
        slmKQLoad = std::max(slmKQLoad, unrollKSLM);

    // SLM quantization parameter loads.
    auto reqSLMLoadQ = every(slmKQLoad)
                     | lookahead(lookaheadSLMStore + lookaheadSLMReload + unrollKSLM - 1);
    if (slmDequantize2D) ls.schedule(reqSLMLoadQ, [&](Iteration h) {
        if (slmDequantize2DA) {
            if (ao2D) gemmALoad(state.A_offsetRegs, state.A_offsetLayout, state.A_offsetAddrs, problem, strategy, state);
            if (as2D) gemmALoad(state.A_scaleRegs,  state.A_scaleLayout,  state.A_scaleAddrs,  problem, strategy, state);
        }
        if (slmDequantize2DB) {
            if (bo2D) gemmBLoad(state.B_offsetRegs, state.B_offsetLayout, state.B_offsetAddrs, problem, strategy, state);
            if (bs2D) gemmBLoad(state.B_scaleRegs,  state.B_scaleLayout,  state.B_scaleAddrs,  problem, strategy, state);
        }
    });

    // SLM loads.
    auto reqSLMLoad = every(unrollKSLM)
                    | variants(slmCopies)
                    | lookahead(lookaheadSLMLoad + lookaheadSLMStore + lookaheadSLMReload);
    auto reqSLMLoadABRem = every(unrollKSLM)
                         | variants(slmCopies)
                         | lookahead(lookaheadSLMLoad + lookaheadSLMStore + lookaheadSLMReloadRem);
    auto reqSLMStore = every(unrollKSLM)
                     | variants(slmCopies)
                     | lookahead(lookaheadSLMStore + lookaheadSLMReload)
                     | duration(durationSLMMainLoad);
    auto reqSLMStoreABRem = every(unrollKSLM)
                          | variants(slmCopies)
                          | lookahead(lookaheadSLMStore + lookaheadSLMReloadRem);

    if ((slmA || slmB) && mustActivateSLMRemainder) {
        ls.schedule({
            {reqSLMLoad | duration(unrollKSLM), nothing},
            {reqSLMLoad | unconditional(), [&](Iteration h) {
                kLoopActivateSLMRemainder(true, false, problem, strategy, state, h.counterOffset());
            }}
        });
    }

    auto doSLMRemLoad = [&](Iteration h) {
        kLoopActivateSLMRemainder(true, false, problem, strategy, state, h.counterOffset());
        if (slmA) gemmAiBiRemLoadInc<true> (h, Ai_incrementalRem, Ai_remIncrCopy, needXPReset, slmRemaskA, kSLMA, Ai_regs(h), state.Ai_layoutRem, state.Ai_addrsRem, state.Ai_layoutK, state.Ai_addrsK, state.Ao_regsRem, state.Ao_layout, problem, strategy, state);
        if (slmB) gemmAiBiRemLoadInc<false>(h, Bi_incrementalRem, Bi_remIncrCopy, needXPReset, slmRemaskB, kSLMB, Bi_regs(h), state.Bi_layoutRem, state.Bi_addrsRem, state.Bi_layoutK, state.Bi_addrsK, state.Bo_regsRem, state.Bo_layout, problem, strategy, state);
    };

    if (slmA || slmB) {
        ls.schedule({
            {reqSLMLoad | duration(unrollKSLM), [&](Iteration h) {
                kLoopActivateSLMRemainder(false, false, problem, strategy, state);
                if (slmA) gemmALoad(Ai_regs(h), state.Ai_layout, state.Ai_addrs, problem, strategy, state);
                if (slmB) gemmBLoad(Bi_regs(h), state.Bi_layout, state.Bi_addrs, problem, strategy, state);
            }},
            {reqSLMLoad | duration(durationSLMMainLoad), doSLMRemLoad},
            {reqSLMLoadABRem,                            doSLMRemLoad}
        });
    }

    // Read suppression W/A for fused EU architectures.
    bool rswaA = strategy.readSuppressionWA && (A_copies == 1) && ((ka_loadMain <= opCountMain) || state.repackA) && state.A_layout.hasMasking();
    bool rswaB = strategy.readSuppressionWA && (B_copies == 1) && ((kb_loadMain <= opCountMain) || state.repackB) && state.B_layout.hasMasking();
    bool rswaARem = strategy.readSuppressionWA && (A_copies == 1) && ((ka_loadRem <= opCountRem) || state.repackARem) && state.A_layoutRem.hasMasking();
    bool rswaBRem = strategy.readSuppressionWA && (B_copies == 1) && ((kb_loadRem <= opCountRem) || state.repackBRem) && state.B_layoutRem.hasMasking();

    Iteration A_lastRSWA;
    bool haveA_lastRSWA = false;

    bool saveRSWA;
    auto disableRSWA = [&](){ saveRSWA = strategy.readSuppressionWA; strategy.readSuppressionWA = false; };
    auto restoreRSWA = [&](){ strategy.readSuppressionWA = saveRSWA; };

    auto doRSWA_A = [&](Iteration h) {
        A_lastRSWA = h;
        haveA_lastRSWA = true;
        doReadSuppressionWA(strategy, state);
    };

    auto doRSWA_B = [&](Iteration h) {
        if (!(haveA_lastRSWA && A_lastRSWA == h))
            doReadSuppressionWA(strategy, state);
        haveA_lastRSWA = false;
    };

    // A/B load scheduling.
    auto reqLoadA = every(ka_loadMain)
                  | duration(ka_loadMain)
                  | variants(A_copies)
                  | lookahead(lookaheadALoad);
    auto reqLoadARem = every(ka_loadRem)
                     | variants(A_copies)
                     | lookahead(lookaheadALoadRem);
    auto reqLoadAPrezero = every(minOPCount)
                         | variants(A_copies)
                         | lookahead(state.repackARem ? 0 : lookaheadALoadRem);

    auto reqLoadB = every(kb_loadMain)
                  | duration(kb_loadMain)
                  | variants(B_copies)
                  | lookahead(lookaheadBLoad);
    auto reqLoadBRem = every(kb_loadRem)
                     | variants(B_copies)
                     | lookahead(lookaheadBLoadRem);
    auto reqLoadBPrezero = every(minOPCount)
                         | variants(B_copies)
                         | lookahead(state.repackBRem ? 0 : lookaheadBLoadRem);

    // A/B prezeroing for partial remainder loads with multi-k outer products.
    bool prezeroARem = !slmA && (ka_loadRem < minOPCount) && readA;
    bool prezeroBRem = !slmB && (kb_loadRem < minOPCount) && readB;

    if (prezeroARem && prezeroBRem && Ta.isInteger() && Tb.isInteger() && !calcASums && !calcBSums) {
        // Only need to pre-zero one operand for integer A/B. Choose the smaller one.
        if (unrollM >= unrollN)
            prezeroARem = false;
        else
            prezeroBRem = false;
    }

    if (prezeroARem) ls.schedule({
        {reqLoadA, nothing},
        {reqLoadAPrezero, [&](Iteration h) {
            zeroMatrix(state.repackARem ? state.Ar_regs : A_regs(h), strategy);
        }}
    });

    if (prezeroBRem) ls.schedule({
        {reqLoadB, nothing},
        {reqLoadBPrezero, [&](Iteration h) {
            zeroMatrix(state.repackBRem ? state.Br_regs : B_regs(h), strategy);
        }}
    });

    if (prezeroARem && prezeroBRem && loadBFirst)
        ls.swapLast2();

    // A/B enforced remainder preparations.
    bool didForceActivateRemA = false, didForceActivateRemB = false;
    if (mustActivateRemainder) {
        ls.schedule_if({
            {reqLoadA, nothing, never},
            {reqLoadARem | unconditional(), [&](Iteration h) {
                kLoopActivateABRemainder(true, true, false, problem, strategy, state, h.counterOffset());
                didForceActivateRemA = true;
            }, [&](Iteration h) { return !didForceActivateRemA; }}
        });
        ls.schedule_if({
            {reqLoadB, nothing, never},
            {reqLoadBRem | unconditional(), [&](Iteration h) {
                kLoopActivateABRemainder(true, false, true, problem, strategy, state, h.counterOffset());
                didForceActivateRemB = true;
            }, [&](Iteration h) { return !didForceActivateRemB; }}
        });
    }

    // A loads.
    if (readA) ls.schedule({
        {reqLoadA, [&](Iteration h) {
            if (rswaA) doRSWA_A(h);
            disableRSWA();
            kLoopActivateABRemainder(false, true, false, problem, strategy, state);
            gemmALoad(A_regs(h), state.A_layout, state.A_addrs, problem, strategy, state);
            restoreRSWA();
        }},
        {reqLoadARem, [&](Iteration h) {
            if (rswaARem) doRSWA_A(h);
            disableRSWA();
            kLoopActivateABRemainder(true, true, false, problem, strategy, state, h.counterOffset());
            gemmALoad(A_regs(h), state.A_layoutRem, state.A_addrsRem, problem, strategy, state);
            restoreRSWA();
        }}
    });

    // B loads.
    if (readB) ls.schedule({
        {reqLoadB, [&](Iteration h) {
            if (rswaB) doRSWA_B(h);
            disableRSWA();
            kLoopActivateABRemainder(false, false, true, problem, strategy, state);
            gemmBLoad(B_regs(h), state.B_layout, state.B_addrs, problem, strategy, state);
            restoreRSWA();
        }},
        {reqLoadBRem, [&](Iteration h) {
            if (rswaBRem) doRSWA_B(h);
            disableRSWA();
            kLoopActivateABRemainder(true, false, true, problem, strategy, state, h.counterOffset());
            gemmBLoad(B_regs(h), state.B_layoutRem, state.B_addrsRem, problem, strategy, state);
            restoreRSWA();
        }}
    });

    if (loadBFirst)
        ls.swapLast2();

    // Stalls to promote thread switches.
    auto reqStall = every(lcm(ka_loadMain, kb_loadMain))
                  | checkOptional();

    if (strategy.stallAfterLoad) ls.schedule(reqStall, [&](Iteration h) {
        if (Tc.isInteger()) {
            mov<float>(1, null, 0.0f);
            sync.nop(SWSB<float>(1));
        } else {
            mov<uint32_t>(1, null, 0);
            sync.nop(SWSB<uint32_t>(1));
        }
    });

    // k decrement and loop check.
    auto reqLoopCheck = every(unrollK)
                      | duration(unrollK);

    if (lateKLoopCheck) {
        int last = unrollK;
        if (state.A_layout.hasFlags()) last = std::min(last, ka_loadMain);
        if (state.B_layout.hasFlags()) last = std::min(last, kb_loadMain);
        if (state.Ap_layout.hasFlags())
            last = std::min(last, 1 + (strategy.prefetchA - 1) % ka_pfStride);
        if (state.Bp_layout.hasFlags())
            last = std::min(last, 1 + (strategy.prefetchB - 1) % kb_pfStride);
        if (state.Ai_layout.hasFlags() || state.Bi_layout.hasFlags()) {
            last = std::min(last, unrollKSLM);
            if (lookaheadSLMReload % unrollKSLM != 0)
                last = std::min(last, lookaheadSLMReload % unrollKSLM);
        }
        if (state.A_offsetLayout.hasFlags())
            last = std::min(last, lcm(ao2DLate ? kaq_loadLate : kaq_load, ka_loadMain) - 1);
        if (state.A_scaleLayout.hasFlags())
            last = std::min(last, lcm(as2DLate ? kaq_loadLate : kaq_load, ka_loadMain) - 1);
        if (state.B_offsetLayout.hasFlags())
            last = std::min(last, lcm(bo2DLate ? kbq_loadLate : kbq_load, kb_loadMain) - 1);
        if (state.B_scaleLayout.hasFlags())
            last = std::min(last, lcm(bs2DLate ? kbq_loadLate : kbq_load, kb_loadMain) - 1);
        reqLoopCheck = reqLoopCheck.delay(unrollK - last);
    }

    ls.schedule_if(reqLoopCheck,
        [&](Iteration h) {
            add(1 | gt | f0[0], state.K, state.K, -unrollK);
            if (lateKLoopCheck) {
                state.raVFlag.lock(state.flagAP);
                if (state.vflagsEnabled())
                    state.activeVFlags[state.flagAP.index()].clear();
            }
        },
        [&](Iteration h) {
            return (curPhase == LoopSequencer::PhaseMainLoop);
        }
    );

    // SLM store address increments.
    auto doSLMStoreInc = [&](Iteration h) {
        int kIncSLMStore = (slmBuffer(h) == slmBuffers - 1) ? -(slmBuffers - 1) : +1;
        kIncSLMStore *= unrollKSLM;
        if (slmA)
            gemmAIncrement(state.Ao_layout, state.Ao_addrs, kIncSLMStore, problem, strategy, state);
        if (slmB)
            gemmBIncrement(state.Bo_layout, state.Bo_addrs, kIncSLMStore, problem, strategy, state);
    };

    if (strategy.slmBuffers >= 2) {
        ls.schedule({
            {(reqSLMStore | duration(durationSLMMainLoad)).delay(1), doSLMStoreInc},
            {reqSLMStoreABRem.delay(1),                              doSLMStoreInc}
        });
    }

    bool delayABInc = strategy.delayABInc && !needXPReset;
    int delaySLMInc = delayABInc ? (unrollKSLM >> 1) : 0;

    // Quantization parameter address increment helpers.
    auto doIncAq = [&](Iteration h, bool late) {
        auto kaInc = kInc(h, late ? state.kaqLate : state.kaqStride, problem.aqGroupK);
        if (late ? ao2DLate : ao2D) incAddrK(state.A_offsetAddrs, true,  kaInc, state.ldao,     state.ldaoIncrements, state.A_offsetLayout, strategy, state);
        if (late ? as2DLate : as2D) incAddrK(state.A_scaleAddrs,  true,  kaInc, state.ldaScale, state.ldasIncrements, state.A_scaleLayout,  strategy, state);
        if (late && ag2DLate)       incAddrK(state.Ag_addrs,      true,  kaInc, state.ldag,     state.ldagIncrements, state.Ag_layout,      strategy, state);
    };

    auto doIncBq = [&](Iteration h, bool late) {
        auto kbInc = kInc(h, late ? state.kbqLate : state.kbqStride, problem.bqGroupK);
        if (late ? bo2DLate : bo2D) incAddrK(state.B_offsetAddrs, false, kbInc, state.ldbo,     state.ldboIncrements, state.B_offsetLayout, strategy, state);
        if (late ? bs2DLate : bs2D) incAddrK(state.B_scaleAddrs,  false, kbInc, state.ldbScale, state.ldbsIncrements, state.B_scaleLayout,  strategy, state);
        if (late && bg2DLate)       incAddrK(state.Bg_addrs,      false, kbInc, state.ldbg,     state.ldbgIncrements, state.Bg_layout,      strategy, state);
    };

    // SLM quantization parameter address increment.
    if (slmDequantize2D) ls.schedule(reqSLMLoadQ.delay(delaySLMInc), [&](Iteration h) {
        if (slmDequantize2DA) doIncAq(h, false);
        if (slmDequantize2DB) doIncBq(h, false);
    });

    // SLM load address increments.
    auto doSLMLoadInc = [&](Iteration h) {
        bool fullLoad = (h.remaining() >= (unrollKSLM - delaySLMInc));
        if (slmA && (fullLoad || !Ai_incrementalRem))
            gemmAIncrement(Ai_layout(h), Ai_addrs(h), kInc(h, unrollKSLM), problem, strategy, state, 0, h);
        if (slmB && (fullLoad || !Bi_incrementalRem))
            gemmBIncrement(Bi_layout(h), Bi_addrs(h), kInc(h, unrollKSLM), problem, strategy, state, 0, h);
    };

    auto checkSLMLoadInc = [&](Iteration h) {
        bool fullLoad = (h.remaining() >= (unrollKSLM - delaySLMInc));
        return (slmA && (fullLoad || !Ai_incrementalRem))
            || (slmB && (fullLoad || !Bi_incrementalRem));
    };

    if (slmA || slmB) {
        ls.schedule_if({
            {(reqSLMLoad | duration(durationSLMMainLoad)).delay(delaySLMInc), doSLMLoadInc, checkSLMLoadInc},
            {reqSLMLoadABRem.delay(delaySLMInc),                              doSLMLoadInc, checkSLMLoadInc}
        });
    }

    // A prefetch address increment.
    int delayAPFInc = delayABInc ? (ka_pfStride >> 1) : 0;

    if (strategy.prefetchA && readA) {
        ls.schedule(reqPFA.delay(delayAPFInc), [&](Iteration h) {
            gemmAIncrement(state.Ap_layout, state.Ap_addrs, kInc(h, ka_pfStride), problem, strategy, state);
        });
    }

    // B prefetch address increment.
    int delayBPFInc = delayABInc ? (kb_pfStride >> 1) : 0;

    if (strategy.prefetchB && readB) {
        ls.schedule(reqPFB.delay(delayBPFInc), [&](Iteration h) {
            gemmBIncrement(state.Bp_layout, state.Bp_addrs, kInc(h, kb_pfStride), problem, strategy, state);
        });
    }

    if (strategy.prefetchA && strategy.prefetchB && loadBFirst)
        ls.swapLast2();

    // A/B L3 prefetch address increments.
    gemmScheduleL3PrefetchIncs(&ls, problem, strategy, state);

    // A/B quantization parameter address increment.
    auto reqIncAq = every(kaq_load);
    auto reqIncBq = every(kbq_load);
    if (readA && dequantize2DA) ls.schedule(reqIncAq, [&](Iteration h) { doIncAq(h, false); });
    if (readB && dequantize2DB) ls.schedule(reqIncBq, [&](Iteration h) { doIncBq(h, false); });

    auto reqIncAqLate = every(kaq_loadLate);
    auto reqIncBqLate = every(kbq_loadLate);
    if (readA && dequantize2DALate) ls.schedule(reqIncAqLate, [&](Iteration h) { doIncAq(h, true); });
    if (readB && dequantize2DBLate) ls.schedule(reqIncBqLate, [&](Iteration h) { doIncBq(h, true); });

    // A address increment.
    int delayAInc = (delayABInc && A_copies > 1) ? (ka_loadMain >> 1) : 0;

    auto ka_inc = [&](Iteration h) {
        auto inc = ka_load(h);
        if (slmA) {
            int kWraparound = unrollKSLM * slmBuffers;
            if ((h + inc) % kWraparound < inc)
                inc -= kWraparound;
            return inc;
        } else
            return kInc(h, inc);
    };

    if (readA) ls.schedule({
        {reqLoadA.delay(delayAInc), [&](Iteration h) {
            gemmAIncrement(state.A_layout, state.A_addrs, ka_inc(h), problem, strategy, state, 0, h);
        }},
        {reqLoadARem, [&](Iteration h) {
            gemmAIncrement(state.A_layoutRem, state.A_addrsRem, ka_inc(h), problem, strategy, state, h % unrollKSLM, h);
        }}
    });

    // B address increment.
    int delayBInc = (delayABInc && B_copies > 1) ? (kb_loadMain >> 1) : 0;

    auto kb_inc = [&](Iteration h) {
        auto inc = kb_load(h);
        if (slmB) {
            int kWraparound = unrollKSLM * slmBuffers;
            if ((h + inc) % kWraparound < inc)
                inc -= kWraparound;
            return inc;
        } else
            return kInc(h, inc);
    };

    if (readB) ls.schedule({
        {reqLoadB.delay(delayBInc), [&](Iteration h) {
            gemmBIncrement(state.B_layout, state.B_addrs, kb_inc(h), problem, strategy, state, 0, h);
        }},
        {reqLoadBRem, [&](Iteration h) {
            gemmBIncrement(state.B_layoutRem, state.B_addrsRem, kb_inc(h), problem, strategy, state, h % unrollKSLM, h);
        }}
    });

    if (loadBFirst)
        ls.swapLast2();

    // A/B remasking in k dimension, during remainder handling.
    bool remaskA = !slmA && readA && (minOPCount > 1) && needsRemask(Ta_load, true,  state.A_layoutRem, problem.A, strategy.A, state.A_lateKRem);
    bool remaskB = !slmB && readB && (minOPCount > 1) && needsRemask(Tb_load, false, state.B_layoutRem, problem.B, strategy.B, state.B_lateKRem);

    if (Ta.isInteger() && Tb.isInteger() && !calcASums && !calcBSums) {
        // Only need to remask one operand for integer A/B. Choose the smaller one.
        // Or, if one of A/B was copied to SLM, remasking is done there.
        if (remaskA && remaskB) {
            if (unrollM >= unrollN)
                remaskA = false;
            else
                remaskB = false;
        } else if (slmA || slmB)
            remaskA = remaskB = false;
    }

    int iremaskA = 0, iremaskB = 1;
    auto Ta_remask = Ta_load, Tb_remask = Tb_load;

    if (remaskA && remaskB && Ta_remask.bits() == Tb_remask.bits())
        iremaskB = iremaskA;        /* A, B can share remasking masks */

    if ((remaskA || remaskB) && problem.backward()) stub();

    int remaskPeriod = lcm(remaskA ? ka_loadRem : 1,
                           remaskB ? kb_loadRem : 1);
    auto reqRemaskSetup = every(remaskPeriod);
    auto reqRemaskA = every(ka_loadRem)
                    | variants(A_copies);
    auto reqRemaskB = every(kb_loadRem)
                    | variants(B_copies);

    if (remaskA || remaskB) ls.schedule({
        {reqRemaskSetup | duration(remaskPeriod), nothing},
        {reqRemaskSetup, [&](Iteration h) {
            if (remaskA) {
                setupTeardownRemask(Ta_remask, iremaskA, false, remaskPeriod, state.K, strategy, state);
                setupTeardownRemask(Ta_remask, iremaskA, true,  remaskPeriod, state.K, strategy, state, -h.counterOffset());
            }
            if (remaskB && iremaskB != iremaskA) {
                setupTeardownRemask(Tb_remask, iremaskB, false, remaskPeriod, state.K, strategy, state);
                setupTeardownRemask(Tb_remask, iremaskB, true,  remaskPeriod, state.K, strategy, state, -h.counterOffset());
            }
        }}
    });

    auto teardownRemasks = [&] {
        if (remaskA)
            setupTeardownRemask(Ta_remask, iremaskA, false, remaskPeriod, state.K, strategy, state);
        if (remaskB && iremaskB != iremaskA)
            setupTeardownRemask(Tb_remask, iremaskB, false, remaskPeriod, state.K, strategy, state);
    };

    if (remaskA) ls.schedule({
        {reqLoadA, nothing},
        {reqRemaskA, [&](Iteration h) {
            remaskLayout(iremaskA, true, state.A_layoutRem, A_regs(h), strategy, state, h % remaskPeriod);
        }}
    });

    if (remaskB) ls.schedule({
        {reqLoadB, nothing},
        {reqRemaskB, [&](Iteration h) {
            remaskLayout(iremaskB, false, state.B_layoutRem, B_regs(h), strategy, state, h % remaskPeriod);
        }}
    });

    if (remaskA && remaskB && loadBFirst)
        ls.swapLast2();

    // A/B quantization parameter repacking and remasking.
    auto reqRepackAq = every(kaq_load);
    auto reqRepackBq = every(kbq_load);
    auto reqRepackAqLate = every(kaq_loadLate);
    auto reqRepackBqLate = every(kbq_loadLate);

    bool remaskAq = (ao2D || as2D) && (minOPCount > 1) && (problem.aqGroupK == 1);
    bool remaskBq = (ao2D || bs2D) && (minOPCount > 1) && (problem.bqGroupK == 1);
    int iremaskScale = 2;

    auto doRemaskAq = [&](Iteration h, bool slm) {
        if (!remaskAq) return;
        Subregister offK;
        int ks = state.A_scaleLayout.cols();
        if (slm && (state.effCoopA == CoopSplit::K || state.effCoopA == CoopSplit::FullK)) {
            offK = state.ra.allocSub<uint32_t>();
            mulConstant(1, offK, state.lidN, state.ka_slm);
        }
        if (as2D) {
            remaskLayoutSingle(iremaskScale, true, ks, state.K,
                               state.A_scaleLayout, state.A_scaleRegs, strategy, state,
                               -h.counterOffset(), offK);
        }
        if (ao2D) {
            remaskLayoutSingle(iremaskScale, true, ks, state.K,
                               state.A_offsetLayout, state.A_offsetRegs, strategy, state,
                               -h.counterOffset(), offK);
        }
        state.ra.safeRelease(offK);
    };

    auto doRemaskBq = [&](Iteration h, bool slm) {
        if (!remaskBq) return;
        Subregister offK;
        int ks = state.B_scaleLayout.rows();
        if (slm && (state.effCoopB == CoopSplit::K || state.effCoopB == CoopSplit::FullK)) {
            offK = state.ra.allocSub<uint32_t>();
            mulConstant(1, offK, state.lidM, state.ka_slm);
        }
        if (bs2D) {
            remaskLayoutSingle(iremaskScale, false, ks, state.K,
                               state.B_scaleLayout, state.B_scaleRegs, strategy, state,
                               -h.counterOffset(), offK);
        }
        if (bo2D) {
            remaskLayoutSingle(iremaskScale, false, ks, state.K,
                               state.B_offsetLayout, state.B_offsetRegs, strategy, state,
                               -h.counterOffset(), offK);
        }
        state.ra.safeRelease(offK);
    };

    auto doRepackAq = [&](Iteration h, bool late) {
        if (!late && A_remActive(h)) doRemaskAq(h, false);
        if (late ? ao2DLate : ao2D) gemmRepack2DOffsetData(Ta_ext, state.A_offsetLayout, state.Ar_offsetLayout, state.A_offsetRegs, state.Ar_offsetRegs, problem, strategy, state);
        if (late ? as2DLate : as2D) gemmRepack2DQuantizationData(state.A_scaleLayout,    state.Ar_scaleLayout,  state.A_scaleRegs,  state.Ar_scaleRegs,  problem, strategy, state);
        if (late && ag2DLate)       gemmRepack2DQuantizationData(state.Ag_layout,        state.Agr_layout,      state.Ag_regs,      state.Agr_regs,      problem, strategy, state);
    };

    auto doRepackBq = [&](Iteration h, bool late) {
        if (!late && B_remActive(h)) doRemaskBq(h, false);
        if (late ? bo2DLate : bo2D) gemmRepack2DOffsetData(Tb_ext, state.B_offsetLayout, state.Br_offsetLayout, state.B_offsetRegs, state.Br_offsetRegs, problem, strategy, state);
        if (late ? bs2DLate : bs2D) gemmRepack2DQuantizationData(state.B_scaleLayout,    state.Br_scaleLayout,  state.B_scaleRegs,  state.Br_scaleRegs,  problem, strategy, state);
        if (late && bg2DLate)       gemmRepack2DQuantizationData(state.Bg_layout,        state.Bgr_layout,      state.Bg_regs,      state.Bgr_regs,      problem, strategy, state);
    };

    if (dequantize2DA)     ls.schedule(reqRepackAq,     [&](Iteration h) { doRepackAq(h, false); });
    if (dequantize2DB)     ls.schedule(reqRepackBq,     [&](Iteration h) { doRepackBq(h, false); });
    if (dequantize2DALate) ls.schedule(reqRepackAqLate, [&](Iteration h) { doRepackAq(h, true);  });
    if (dequantize2DBLate) ls.schedule(reqRepackBqLate, [&](Iteration h) { doRepackBq(h, true);  });

    // A/B repacking.
    auto reqRepackA = every(ka_repackMain)
                    | variants(A_copies);
    auto reqRepackARem = every(std::min(ka_loadRem, ka_repackRem))
                       | variants(A_copies);
    bool convertA = (Ta != Ta_load) && (Ta.bits() == Ta_load.bits());
    bool scheduleRepackA = state.repackA || state.repackARem || convertA || dequantizeA;

    auto doRepackA = [&](RegisterLayout &layout, GRFMultirange &regs, bool repackA, int h, int k_load, int k_repack) {
        k_repack = std::max(k_repack, 1);
        int ha = h % k_load;
        int har = h % k_repack;

        auto sublayout = layout;
        auto Ar_sublayout = state.Ar_layout;
        bool s4Shift = true;

        int hq = kaq_load ? (har % kaq_load) : 0;
        if (repackA) {
            auto layoutCopy = layout;
            layoutCopy.unlinkFromMemory();
            sublayout = layoutCopy.slice(true, ha, ha + k_repack, false);
            for (auto &l: Ar_sublayout)
                l.offsetC += ha;

            // Int4 data is commonly expanded from partial registers as a 64
            // byte register expands to 128 elements. To avoid emitting extra
            // instructions, perform element-wise operations here.
            if (canDequantizeInt4(layout, state.Ar_layout, {}, {})) {
                if (ha == 0) dequantizeInt4Shift(Ta_load, regs, strategy);
                s4Shift = false;
                hq = 0;
            }
        }
        if (dequantizeA)
            gemmDequantizeAB(true, sublayout, Ar_sublayout, regs, state.Ar_regs, har, hq, problem, strategy, state, s4Shift);
        else
        if (repackA)
            copyRegisters(sublayout, Ar_sublayout, regs, state.Ar_regs, 0, har, false, strategy, state, false, s4Shift);
        else if (convertA)
            convert(regs, Ta_load, Ta, strategy, state);
    };

    if (scheduleRepackA && readA) ls.schedule({
        {reqRepackA,    [&](Iteration h) { doRepackA(state.A_layout,    A_regs(h), state.repackA,    h, ka_loadMain, ka_repackMain); }},
        {reqRepackARem, [&](Iteration h) { doRepackA(state.A_layoutRem, A_regs(h), state.repackARem, h, ka_loadRem,  ka_repackRem);  }}
    });

    auto reqRepackB = every(kb_loadMain)
                    | variants(B_copies);
    auto reqRepackBRem = every(kb_loadRem)
                       | variants(B_copies);
    bool convertB = (Tb != Tb_load) && (Tb.bits() == Tb_load.bits());
    bool scheduleRepackB = state.repackB || state.repackBRem || convertB || dequantizeB;

    auto doRepackB = [&](RegisterLayout &layout, GRFMultirange &regs, bool repackB, int h, int hb) {
        if (dequantizeB)
            gemmDequantizeAB(false, layout, state.Br_layout, regs, state.Br_regs, hb, h % kbq_load, problem, strategy, state);
        else
        if (repackB)
            copyRegisters(layout, state.Br_layout, regs, state.Br_regs, hb, 0, false, strategy, state);
        else if (convertB)
            convert(regs, Tb_load, Tb, strategy, state);
    };

    if (scheduleRepackB && readB) ls.schedule({
        {reqRepackB,    [&](Iteration h) { doRepackB(state.B_layout,    B_regs(h), state.repackB,    h, 0);                                   }},
        {reqRepackBRem, [&](Iteration h) { doRepackB(state.B_layoutRem, B_regs(h), state.repackBRem, h, h % std::max(state.kb_repackRem, 1)); }}
    });

    if (scheduleRepackA && scheduleRepackB && loadBFirst)
        ls.swapLast2();

    // A/B 2D quantization parameter loads.
    auto reqLoadAq = every(kaq_load) | lookahead(ka_repackMain);
    auto reqLoadBq = every(kbq_load) | lookahead(kb_loadMain);
    auto reqLoadAqLate = every(kaq_loadLate) | lookahead(kaq_loadLate);
    auto reqLoadBqLate = every(kbq_loadLate) | lookahead(kbq_loadLate);

    auto doLoadAq = [&](Iteration h, bool late) {
        if (late ? ao2DLate : ao2D) gemmALoad(state.A_offsetRegs, state.A_offsetLayout, state.A_offsetAddrs, problem, strategy, state);
        if (late ? as2DLate : as2D) gemmALoad(state.A_scaleRegs,  state.A_scaleLayout,  state.A_scaleAddrs,  problem, strategy, state);
        if (late && ag2DLate)       gemmALoad(state.Ag_regs,      state.Ag_layout,      state.Ag_addrs,      problem, strategy, state);
    };

    auto doLoadBq = [&](Iteration h, bool late) {
        if (late ? bo2DLate : bo2D) gemmBLoad(state.B_offsetRegs, state.B_offsetLayout, state.B_offsetAddrs, problem, strategy, state);
        if (late ? bs2DLate : bs2D) gemmBLoad(state.B_scaleRegs,  state.B_scaleLayout,  state.B_scaleAddrs,  problem, strategy, state);
        if (late && bg2DLate)       gemmBLoad(state.Bg_regs,      state.Bg_layout,      state.Bg_addrs,      problem, strategy, state);
    };

    if (readA && dequantize2DA)     ls.schedule(reqLoadAq,     [&](Iteration h) { doLoadAq(h, false); });
    if (readB && dequantize2DB)     ls.schedule(reqLoadBq,     [&](Iteration h) { doLoadBq(h, false); });
    if (readA && dequantize2DALate) ls.schedule(reqLoadAqLate, [&](Iteration h) { doLoadAq(h, true);  });
    if (readB && dequantize2DBLate) ls.schedule(reqLoadBqLate, [&](Iteration h) { doLoadBq(h, true);  });

    // Outer product(s).
    // If outer products batched across k (dp4a/dpas/k-chaining), trigger every opCount loops.
    auto reqOP = every(minOPCount)
               | lookahead(-(minOPCount - 1));

    int ka_sumMain = !state.A_layout.colMajor() ? ka_loadMain : opCountMain;
    int kb_sumMain =  state.B_layout.colMajor() ? kb_loadMain : opCountMain;

    ls.schedule(reqOP, [&](Iteration h) {
        auto oc = opCount(h);
        auto hNext = h + minOPCount;
        if (hNext % oc != 0)
            return;

        int ka = ka_repack(h), kb = kb_load(h);
        int ha = h % ka;
        int hb = h % kb;
        if (problem.backward()) {
            ha = ka - 1 - ha;
            hb = kb - 1 - hb;
        }

        auto &layoutA = Ar_layout(h);
        auto &layoutB = Br_layout(h);
        auto &regsA = Ar_regs(h);
        auto &regsB = Br_regs(h);

            outerProduct(h, ha, hb, oc, opRemActive(h), layoutA, layoutB, regsA, regsB, problem, strategy, state);

        if (calcASums && !slmASums && !state.systolicSumA) {
            int ka_sum = (curPhase == LoopSequencer::PhaseMainLoop) ? ka_sumMain : oc;
            int ha0 = ha - oc + minOPCount;
            if (ha0 % ka_sum == 0)
                accumulateSum(false, regsA, layoutA, state.As_regs, state.As_layout, strategy, state, ha0, ha0 + ka_sum);
        }

        if (calcBSums && !slmBSums && !state.systolicSumB) {
            int kb_sum = (curPhase == LoopSequencer::PhaseMainLoop) ? kb_sumMain : oc;
            int hb0 = hb - oc + minOPCount;
            if (hb0 % kb_sum == 0)
                accumulateSum(true, regsB, layoutB, state.Bs_regs, state.Bs_layout, strategy, state, hb0, hb0 + kb_sum);
        }
    });

    // Late A/B grouped offsets.
    //   If C is repacked, offsets are applied during that step instead of here.
    bool doAO2DLate = bg2DLate && state.Cr_layout.empty();
    bool doBO2DLate = ag2DLate && state.Cr_layout.empty();
    if (doAO2DLate || doBO2DLate) {
        int period = lcm(aqGroupK, bqGroupK);
        auto reqXg = every(period)
                   | lookahead(-(period - 1));
        ls.schedule(reqXg, [&](Iteration h) {
            if (doAO2DLate) applyLateABOffset(true,  h, problem, strategy, state);
            if (doBO2DLate) applyLateABOffset(false, h, problem, strategy, state);
        });
    }

    // SLM quantization parameter repacking.
    auto reqSLMRepackQ = every(slmKQLoad)
                       | lookahead(lookaheadSLMStore + lookaheadSLMReload + strategy.slmRepackAhead);

    if (slmDequantize2D) ls.schedule(reqSLMRepackQ, [&](Iteration h) {
        if (slmDequantize2DA) {
            if (slmRemActive(h)) doRemaskAq(h, true);
            if (ao2D) gemmRepack2DOffsetData(Ta_ext, state.A_offsetLayout, state.Ar_offsetLayout, state.A_offsetRegs, state.Ar_offsetRegs, problem, strategy, state);
            if (as2D) gemmRepack2DQuantizationData(state.A_scaleLayout,    state.Ar_scaleLayout,  state.A_scaleRegs,  state.Ar_scaleRegs,  problem, strategy, state);
        }
        if (slmDequantize2DB) {
            if (slmRemActive(h)) doRemaskBq(h, true);
            if (bo2D) gemmRepack2DOffsetData(Tb_ext, state.B_offsetLayout, state.Br_offsetLayout, state.B_offsetRegs, state.Br_offsetRegs, problem, strategy, state);
            if (bs2D) gemmRepack2DQuantizationData(state.B_scaleLayout,    state.Br_scaleLayout,  state.B_scaleRegs,  state.Br_scaleRegs,  problem, strategy, state);
        }
    });

    // SLM data repacking and remasking.
    auto reqSLMRepack = every(unrollKSLM)
                      | variants(slmCopies)
                      | lookahead(lookaheadSLMStore + lookaheadSLMReload + strategy.slmRepackAhead)
                      | duration(durationSLMMainLoad);
    auto reqSLMRepackABRem = every(unrollKSLM)
                           | variants(slmCopies)
                           | lookahead(lookaheadSLMStore + lookaheadSLMReloadRem + strategy.slmRepackAhead);

    auto slmConvertA = [&](Iteration h) { return slmA && aioShare(h) && (Ta != Ta_ext) && (Ta.bits() == Ta_ext.bits()); };
    auto slmConvertB = [&](Iteration h) { return slmB && bioShare(h) && (Tb != Tb_ext) && (Tb.bits() == Tb_ext.bits()); };

    auto doSLMRepack = [&](Iteration h) {
        if (slmDequantizeA)
            gemmDequantizeAB(true, Ai_layout(h), state.Ao_layout, Ai_regs(h), Ao_regs(h), 0, 0, problem, strategy, state);
        else
        if (slmA && !aioShare(h) && !(slmRemActive(h) && Ai_remIncrCopy))
            copyRegisters(Ai_layout(h), state.Ao_layout, Ai_regs(h), Ao_regs(h), strategy, state);
        else if (slmConvertA(h))
            convert(Ai_regs(h), Ta_ext, Ta, strategy, state);

        if (slmDequantizeB)
            gemmDequantizeAB(false, Bi_layout(h), state.Bo_layout, Bi_regs(h), Bo_regs(h), 0, 0, problem, strategy, state);
        else
        if (slmB && !bioShare(h) && !(slmRemActive(h) && Bi_remIncrCopy))
            copyRegisters(Bi_layout(h), state.Bo_layout, Bi_regs(h), Bo_regs(h), strategy, state);
        else if (slmConvertB(h))
            convert(Bi_regs(h), Tb_ext, Tb, strategy, state);

        if (slmRemActive(h) && (slmRemaskA || slmRemaskB)) {
            releaseMaskAssignments(kMasksAi, state);  // Not in use -- can temporarily free these.
            releaseMaskAssignments(kMasksBi, state);
            gemmSLMRemask(slmRemaskA, slmRemaskB, effAo_regs(h), effBo_regs(h), -h.counterOffset(), problem, strategy, state);
            reclaimMaskAssignments(kMasksAi, state);
            reclaimMaskAssignments(kMasksBi, state);
        }
    };

    auto checkSLMRepack = [&](Iteration h) {
        return (slmA && !aioShare(h) && !(slmRemActive(h) && Ai_remIncrCopy))
            || (slmB && !bioShare(h) && !(slmRemActive(h) && Bi_remIncrCopy))
            || (slmRemActive(h) && (slmRemaskA || slmRemaskB))
            || slmConvertA(h)
            || slmConvertB(h);
    };

    if (slmA || slmB) {
        ls.schedule_if({
            {reqSLMRepack,      doSLMRepack, checkSLMRepack},
            {reqSLMRepackABRem, doSLMRepack, checkSLMRepack}
        });
    }

    // SLM stores and synchronization.
    auto reqSLMAfterStore = every(unrollKSLM)
                          | variants(slmCopies)
                          | lookahead(lookaheadSLMStore + lookaheadSLMReload - unrollKSLM)
                          | duration(durationSLMMainLoad);
    auto reqSLMAfterStore2 = every(unrollKSLM)
                           | variants(slmCopies)
                           | lookahead(lookaheadSLMStore + lookaheadSLMReload - 2 * unrollKSLM)
                           | duration(durationSLMMainLoad);
    auto reqSLMAfterStoreABRem = every(unrollKSLM)
                               | variants(slmCopies)
                               | lookahead(lookaheadSLMStore + lookaheadSLMReloadRem - unrollKSLM);
    auto reqSLMAfterStoreABRem2 = every(unrollKSLM)
                                | variants(slmCopies)
                                | lookahead(lookaheadSLMStore + lookaheadSLMReloadRem - 2 * unrollKSLM);

    auto slm1x2xFencedBarrier = [&]() {
        // For DG2+, before 1x/2x buffered stores, we must ensure prior SLM reads are complete.
        // Use a fence for >2x global buffering.
        // For 2x global buffering, use SWSB since loaded data will be used shortly.
        // For 1x global buffering, loaded data has already been consumed.
        if (hw < HW::XeHPG && !strategy.strictFence)
            kLoopBarrier(false, KBarrierType::Normal);
        else if ((A_copies > 2 || B_copies > 2) && !strategy.slmFenceWARWA)
            kLoopBarrier(true, KBarrierType::Normal);
        else {
            if (slmA && A_copies > 1) wrdepRanges(state.A_regs);
            if (slmB && B_copies > 1) wrdepRanges(state.B_regs);
            kLoopBarrier(false, KBarrierType::Normal);
        }
    };

    auto doSLMAfterStore2 = [&](Iteration h) {
        switch (slmBuffers) {
            case 1:
            case 2:
            case 3: break;
            case 4:
                kLoopBarrier(false, KBarrierType::Wait);
                break;
            default: stub();
        }
    };

    auto doSLMAfterStore = [&](Iteration h) {
        switch (slmBuffers) {
            case 1: break;
            case 2:
                slm1x2xFencedBarrier();
                break;
            case 3:
                kLoopBarrier(false, KBarrierType::Wait);
                break;
            case 4:
                // TEMP: move me earlier.
                slmFenceIssue();
                //
                fencewait();
                if (strategy.slmFenceWARWA) {
                    // Work around buggy SLM fence by ensuring SLM reads complete.
                    if (slmA && A_copies > 1) wrdepRanges(state.A_regs);
                    if (slmB && B_copies > 1) wrdepRanges(state.B_regs);
                }
                kLoopBarrier(false, KBarrierType::Signal);
                break;
        }
    };

    auto doSLMStore = [&](Iteration h) {
        if (!slmA && !slmB) return;

        switch (slmBuffers) {
            case 1:
                slm1x2xFencedBarrier();
                break;
            case 2:
            case 3:
            case 4: break;
            default: stub();
        }

        {
            if (slmA) storeMatrix(effAo_regs(h), state.Ao_layout, state.Ao_addrs, strategy, state);
            if (slmB) storeMatrix(effBo_regs(h), state.Bo_layout, state.Bo_addrs, strategy, state);
        }

        if (slmASums) accumulateSum(false, effAo_regs(h), state.Ao_layout, state.As_regs, state.As_layout, strategy, state);
        if (slmBSums) accumulateSum(true,  effBo_regs(h), state.Bo_layout, state.Bs_regs, state.Bs_layout, strategy, state);

        switch (slmBuffers) {
            case 1:
                kLoopBarrier(true, KBarrierType::Normal);
                break;
            case 2:
                slmFenceIssue();
                fencewait();
                break;
            case 3:
                if (strategy.slmFenceWARWA) {
                    // Work around buggy SLM fence by ensuring SLM reads complete.
                    // Should be moved later, just before the barrier.
                    if (slmA && A_copies > 1) wrdepRanges(state.A_regs);
                    if (slmB && B_copies > 1) wrdepRanges(state.B_regs);
                }
                kLoopBarrier(true, KBarrierType::Signal);
                break;
            case 4: break;
            default: stub();
        }
    };

    if (slmBuffers > 0) {
        if (slmBuffers >= 4) ls.schedule({
            {reqSLMAfterStore2,      doSLMAfterStore2},
            {reqSLMAfterStoreABRem2, doSLMAfterStore2}
        });

        if (slmBuffers >= 2) ls.schedule({
            {reqSLMAfterStore,      doSLMAfterStore},
            {reqSLMAfterStoreABRem, doSLMAfterStore}
        });

        ls.schedule({
            {reqSLMStore,      doSLMStore},
            {reqSLMStoreABRem, doSLMStore}
        });
    }

    // Periodic barriers, if occurring at least once per unrollK.
    if (barrierTask) {
        auto reqBarrier = every(strategy.barrierFreq)
                        | phase(strategy.barrierFreq - 1)
                        | unconditional();
        ls.schedule(reqBarrier, [&](Iteration h) {
            if (curPhase == LoopSequencer::PhaseMainLoop) {
                if (strategy.splitBarrier) {
                    kLoopBarrier(false, KBarrierType::Wait);
                    kLoopBarrier(false, KBarrierType::Signal);
                } else
                    kLoopBarrier(false, KBarrierType::Normal);
            }
        });
    }

    // Save pre-loop state.
    auto statePreLoop = state;

    using CT = LoopSequencer::CallbackType;

    Label lTop, lBottom, lNextTilePFL3;
    std::vector<Label> labels;

    ls.analyze();

    if (ls.getUnroll() != unrollK) stub();  // Auto-calculated unroll should match unrollK from strategy.

    // Prepare to save off loops for periodic barriers, if needed.
    Subregister outerK;
    if (barrierSubloop)
        outerK = state.ra.alloc_sub<uint32_t>();

    // Prepare to peel loops for L3 prefetch, if needed.
    Subregister l3PFPeelK;
    if (strategy.prefetchABL3)
        l3PFPeelK = state.ra.alloc_sub<uint32_t>();

    // Prepare to peel loops for C prefetch, if needed.
    int prefetchCPeelLoops = -1;
    Subregister pfCPeelK;
    if (strategy.prefetchC > 0) {
        prefetchCPeelLoops = div_up(std::max(0, strategy.prefetchC - ls.getCooldown()), unrollK);
        if (prefetchCPeelLoops > 0)
            pfCPeelK = state.ra.alloc_sub<uint32_t>();
    }

    // Virtual flag teardown.
    bool hadVFlags = state.vflagsEnabled();
    auto vflagTeardown = [&]() {
        if (state.vflagsEnabled() && !hadVFlags)
            deallocVFlagStorage(state);
    };

    // Events when resetting for a new loop.
    auto resetForNewLoop = [&]() {
        resetKSLM();
        lastThresh = 0;
        haveA_lastRSWA = false;
        state.ra.safeRelease(barrierHeader);
        teardownRemasks();
        didForceActivateRemA = didForceActivateRemB = false;
    };

    // Main events in lifetime of loop.
    ls.setCallback(CT::OffsetCounter, [&](int offset, int) {
        add(1, state.K, state.K, offset);
    });
    ls.setCallback(CT::LoopStart, [&](int unroll, int) {
        if (strategy.prefetchABL3) {
            int peel = strategy.prefetchABL3 - ls.getLoopBias();
            if (peel < unroll) {
                peel = unroll;
                status << "Warning: L3 prefetch distance too short for k loop; extending" << status_stream::endl;
            }
            add(1 | le | state.flagAP, state.K, state.K, -peel);
            mov(1, l3PFPeelK, peel);
        } else
            cmp(1 | le | state.flagAP, state.K, 0);
        if (prefetchCPeelLoops > 0) {
            min_(1, pfCPeelK, state.K, prefetchCPeelLoops * unrollK);
            add(1, state.K, state.K, -pfCPeelK);
        }
        if (barrierSubloop) {
            if (state.kNoBarrierStart.isValid())
                add(1, state.K, state.K, -state.kNoBarrierStart);
            if (state.kNoBarrierEnd.isValid())
                add(1, state.K, state.K, -state.kNoBarrierEnd);
            add(1 | sat, outerK, state.K, -strategy.barrierFreq);
            min_(1, state.K, state.K, strategy.barrierFreq);
            if (strategy.splitBarrier)
                kLoopBarrier(false, KBarrierType::Signal);
            if (state.kNoBarrierStart.isValid())
                add(1, state.K, state.K, state.kNoBarrierStart);
        } else if (barrierTask && strategy.splitBarrier)
            kLoopBarrier(false, KBarrierType::Signal);
        if (hw == HW::Gen12LP)
            sync.nop(SWSB(1));
        else if (hw > HW::Gen12LP)
            sync.nop(SWSB(Pipe::A, 1));
        jmpi(1 | state.flagAP, strategy.prefetchABL3 ? lNextTilePFL3 : lBottom);
        mark(lTop);
        state.wipeActiveVFlags();
    });
    ls.setCallback(CT::LoopEnd, [&](int, int) {
        jmpi(1 | state.flagAP, lTop);
        if (barrierSubloop) {
            Label lOut;
            add(1, state.K, state.K, outerK);
            add(1 | sat, outerK, outerK, int16_t(-strategy.barrierFreq));
            add(1 | gt | state.flagAP, state.K, state.K, -outerK);
            if (noFinalBarrier)
                jmpi(1 | ~state.flagAP, lOut);
            if (strategy.splitBarrier) {
                kLoopBarrier(false, KBarrierType::Wait);
                kLoopBarrier(false, KBarrierType::Signal);
            } else
                kLoopBarrier(false, KBarrierType::Normal);
            jmpi(1 | state.flagAP, lTop);
            if (noFinalBarrier)
                mark(lOut);
            if (state.kNoBarrierEnd.isValid()) {
                add(1 | gt | state.flagAP, state.K, state.K, state.kNoBarrierEnd);
                mov(1, state.kNoBarrierEnd, 0);
                jmpi(1 | state.flagAP, lTop);
            }
        }
        if (prefetchCPeelLoops > 0) {
            add(1 | gt | state.flagAP, state.K, state.K, pfCPeelK);
            mov(1, pfCPeelK, 0);
            gemmPrefetchC(problem, strategy, state);
            jmpi(1 | state.flagAP, lTop);
        }
        mark(lBottom);
        if (strategy.prefetchABL3) {
            Label lPeelDone;
            cmp(1 | eq | state.flagAP, l3PFPeelK, 0);
            jmpi(1 | state.flagAP, lPeelDone);
            mark(lNextTilePFL3);
            /* Start L3 prefetch for next tile */
            gemmInitL3Prefetch(true, problem, strategy, state);
            add(1 | le | state.flagAP, state.K, state.K, l3PFPeelK);
            mov(1, l3PFPeelK, 0);
            jmpi(1 | ~state.flagAP, lTop);
            mark(lPeelDone);
        }
        state.wipeActiveVFlags();
    });
    ls.setCallback(CT::JumpIfLT, [&](int thresh, int label) {
        if (size_t(label) >= labels.size())
            labels.resize(label + 1);
        if (thresh != lastThresh)
            cmp(1 | lt | state.flagAP, state.K, thresh);
        jmpi(1 | state.flagAP, labels[label]);
        lastThresh = thresh;
    });
    ls.setCallback(CT::JumpTarget, [&](int label, int) {
        mark(labels[label]);
        state.wipeActiveVFlags();
    });
    ls.setCallback(CT::Jump, [&](int label, int) {
        if (size_t(label) >= labels.size())
            labels.resize(label + 1);
        jmpi(1, labels[label]);
    });
    ls.setCallback(CT::NotifyPhase, [&](int phase, int) {
        curPhase = phase;
        switch (phase) {
            case LoopSequencer::PhaseWarmup:
                status << "k loop warmup" << status_stream::endl;
                break;
            case LoopSequencer::PhaseMainLoop:
                status << "Main k loop" << status_stream::endl;
                break;
            case LoopSequencer::PhaseMainPathEnd:
                vflagTeardown();
                if (strategy.barrierFreq > 0 && strategy.splitBarrier)
                    kLoopBarrier(false, KBarrierType::Wait);
                break;
            case LoopSequencer::PhaseCooldown:
                if (state.kNoBarrierEnd.isValid())
                    add(1, state.K, state.K, state.kNoBarrierEnd);
                if (prefetchCPeelLoops == 0)
                    gemmPrefetchC(problem, strategy, state);
                haveA_lastRSWA = false;
                status << "k loop cooldown" << status_stream::endl;
                break;
            case LoopSequencer::PhaseShortLoop:
                if (strategy.prefetchC > 0)
                    gemmPrefetchC(problem, strategy, state);
                if (strategy.prefetchABL3)
                    gemmInitL3Prefetch(true, problem, strategy, state);
                status << "Short k loop" << status_stream::endl;
                remActiveA = remActiveB = remActiveSLM = false;
                resetForNewLoop();
                state = statePreLoop;
                if (state.splitBarrierAlways && strategy.barrierFreq > 0 && strategy.splitBarrier)
                    kLoopBarrier(false, KBarrierType::Signal);
                break;
            case LoopSequencer::PhaseShortLoopEnd:
                vflagTeardown();
                if (state.splitBarrierAlways && strategy.barrierFreq > 0 && strategy.splitBarrier)
                    kLoopBarrier(false, KBarrierType::Wait);
                break;
            case LoopSequencer::PhaseRemainder:
                status << "k loop remainder" << status_stream::endl;
                break;
            default: break;
        }
    });

    // Early C prefetch.
    if (strategy.prefetchC < 0)
        gemmPrefetchC(problem, strategy, state);

    // Generate k loop.
            if (lateKLoopCheck)
                state.raVFlag.unlock(state.flagAP);
            syncall();  /* Avoid unnecessary SWSB dependencies entering loop. */
            ls.materialize();

    // Release barrier header from short k loop.
    state.ra.safeRelease(barrierHeader);

    // Additional barriers to match other threads' barrier count, if other threads might have different k.
    if (matchBarriers) {
        status << "Match barrier counts between threads" << status_stream::endl;
        Subregister myBarriers, k0Barriers;
        Label lSkipExtraBarriers, lExtraBarrierLoop;
        int maxExtraBarriers = 0;
        if (strategy.slmBuffers == 2)
            maxExtraBarriers = div_up(strategy.unroll[LoopK], strategy.unrollKSLM);

        if (strategy.barrierFreq > 0 && prefetchCPeelLoops > 0) stub();

        gemmCalcKLoopBarrierCount(k0Barriers, state.threadK0, ls.getCooldown(), problem, strategy, state);
        gemmCalcKLoopBarrierCount(myBarriers, state.k,        ls.getCooldown(), problem, strategy, state);
        if (maxExtraBarriers > 0)
            add(1, k0Barriers, k0Barriers, maxExtraBarriers);
        add(1 | sat | le | state.flagAP, myBarriers.ud(), k0Barriers, -myBarriers);
        (void) kLoopGetBarrierHeader(strategy, state);
        jmpi(1 | state.flagAP, lSkipExtraBarriers);

        mark(lExtraBarrierLoop); {
            add(1 | gt | state.flagAP, myBarriers, myBarriers, -1);
            kLoopBarrier(false, KBarrierType::Normal);
            jmpi(1 | state.flagAP, lExtraBarrierLoop);
        }
        mark(lSkipExtraBarriers);

        state.ra.safeRelease(myBarriers);
        state.ra.safeRelease(k0Barriers);
        if (!strategy.persistentLoop() && !strategy.fuseBeta && !strategy.kParallelVariable) {
            state.ra.safeRelease(state.threadK0);
            state.ra.safeRelease(state.inputs.k0);
        }
    }

    // Free resources that are no longer needed.
    state.ra.safeRelease(outerK);
    state.ra.safeRelease(pfCPeelK);
    teardownRemasks();
    resetKSLM();

    state.firstKLoopSegment = false;

    if (state.A_descRem) setupTeardownLoadStoreDesc(false, state.A_layoutRem, strategy, state);
    if (state.B_descRem) setupTeardownLoadStoreDesc(false, state.B_layoutRem, strategy, state);

    // If lda/b were duplicated in remainder loops, free them
    //  as duplicates may not be consistent between across short and remainder loops.
    if (!statePreLoop.lda.isDuplicated()) deduplicateScalar(state.lda, state);
    if (!statePreLoop.ldb.isDuplicated()) deduplicateScalar(state.ldb, state);

    // Similarly vflags may not be consistent.
    state.wipeActiveVFlags();
}

// Increment A pointer after load, inside GEMM k loop.
template <HW hw>
void Generator<hw>::gemmAIncrementInternal(const RegisterLayout &layout, const std::vector<GRFRange> &addrs, int ka_inc,
                                           const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int ha)
{
    auto Ta = layout.type();
    const auto &atype = layout.addressing();
    const auto &astrategy = layout.addressingStrategy();

    if (ka_inc == 0)
        /* no-op */;
    else if (astrategy.address2D)
        incDecAddr(addrs, Subregister(), 0, ka_inc, layout, strategy, state, problem.backward());
    else if (atype.layout == MatrixLayout::N) {
        bool release = false;
        auto lda_ka = lookupIncrement(state.ldaIncrements, state.lda, ka_inc, strategy, state, &release);
        incDecAddr(addrs, lda_ka, layout, strategy, state, problem.backward());
        if (release) state.ra.safeRelease(lda_ka);
    } else {
        int incA;
        switch (atype.layout) {
            case MatrixLayout::Pc: incA = untile(Ta, atype, 0, 0, ha + ka_inc, atype.packSize, strategy.unrollKSLM)
                                        - untile(Ta, atype, 0, 0, ha,          atype.packSize, strategy.unrollKSLM); break;
            case MatrixLayout::T:  incA = ka_inc; break;
            default: stub();
        }
        incDecAddr(addrs, incA * Ta, layout, strategy, state, problem.backward());
    }
}

template <HW hw>
void Generator<hw>::gemmAIncrementInternal(const RegisterLayout &layout, const std::vector<GRFRange> &addrs, const MultishiftSubregister &ka_inc,
                                           const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int ha)
{
    gemmAIncrementInternal(layout, addrs, ka_inc >> 0, problem, strategy, state, ha);
}

template <HW hw>
void Generator<hw>::gemmAIncrementInternal(const RegisterLayout &layout, const std::vector<GRFRange> &addrs, const Subregister &ka_inc,
                                           const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int ha)
{
    auto Ta = layout.type();
    const auto &atype = layout.addressing();
    const auto &astrategy = layout.addressingStrategy();
    auto ka_bytes = state.ra.alloc_sub<int32_t>();

    if (!astrategy.address2D) switch (atype.layout) {
        case MatrixLayout::N:  emul(1, ka_bytes, state.inputs.lda, ka_inc, strategy, state); break;
        case MatrixLayout::T:  emulConstant(1, ka_bytes, ka_inc, Ta, strategy, state); break;
        case MatrixLayout::Pc: mulConstant(1, ka_bytes, ka_inc, atype.packSize * Ta); break;
        default: stub();
    }

    incDecAddr(addrs, ka_bytes, 0, ka_inc, layout, strategy, state, problem.backward());

    state.ra.safeRelease(ka_bytes);
}

template <HW hw>
template <typename I>
void Generator<hw>::gemmAIncrement(const RegisterLayout &layout, const std::vector<GRFRange> &addrs, I ka_inc,
                                   const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int ha, int h)
{
        gemmAIncrementInternal(layout, addrs, ka_inc, problem, strategy, state, ha);
}

// A load for GEMM k loop.
template <HW hw>
void Generator<hw>::gemmALoad(const GRFMultirange &regs, const RegisterLayout &layout, const std::vector<GRFRange> &addrs,
                              const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state)
{
    loadMatrix(regs, layout, addrs, strategy, state);
}

template <HW hw>
template <typename I>
void Generator<hw>::gemmALoadInc(const GRFMultirange &regs, const RegisterLayout &layout, const std::vector<GRFRange> &addrs, I ka_inc,
                                 const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state)
{
    gemmALoad(regs, layout, addrs, problem, strategy, state);
    gemmAIncrement(layout, addrs, ka_inc, problem, strategy, state);
}

template <HW hw>
void Generator<hw>::gemmBIncrementInternal(const RegisterLayout &layout, const std::vector<GRFRange> &addrs, int kb_inc,
                                           const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int hb)
{
    auto Tb = layout.type();
    const auto &atype = layout.addressing();
    const auto &astrategy = layout.addressingStrategy();
    if (kb_inc == 0)
        /* no-op */;
    else if (astrategy.address2D)
        incDecAddr(addrs, Subregister(), kb_inc, 0, layout, strategy, state, problem.backward());
    else if (atype.layout == MatrixLayout::T) {
        bool release = false;
        auto ldb_kb = lookupIncrement(state.ldbIncrements, state.ldb, kb_inc, strategy, state, &release);
        incDecAddr(addrs, ldb_kb, layout, strategy, state, problem.backward());
        if (release) state.ra.safeRelease(ldb_kb);
    } else {
        int incB;
        switch (atype.layout) {
            case MatrixLayout::Pr: incB = untile(Tb, atype, 0, hb + kb_inc, 0, strategy.unrollKSLM, atype.packSize)
                                        - untile(Tb, atype, 0, hb,          0, strategy.unrollKSLM, atype.packSize); break;
            case MatrixLayout::N:  incB = kb_inc; break;
            default: stub();
        }
        incDecAddr(addrs, incB * Tb, layout, strategy, state, problem.backward());
    }
}

template <HW hw>
void Generator<hw>::gemmBIncrementInternal(const RegisterLayout &layout, const std::vector<GRFRange> &addrs, const MultishiftSubregister &kb_inc,
                                           const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int hb)
{
    gemmBIncrementInternal(layout, addrs, kb_inc >> 0, problem, strategy, state, hb);
}

template <HW hw>
void Generator<hw>::gemmBIncrementInternal(const RegisterLayout &layout, const std::vector<GRFRange> &addrs, const Subregister &kb_inc,
                                           const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int hb)
{
    auto Tb = layout.type();
    const auto &atype = layout.addressing();
    const auto &astrategy = layout.addressingStrategy();
    auto kb_bytes = state.ra.alloc_sub<int32_t>();

    if (!astrategy.address2D) switch (atype.layout) {
        case MatrixLayout::T:  emul(1, kb_bytes, state.inputs.ldb, kb_inc, strategy, state); break;
        case MatrixLayout::N:  emulConstant(1, kb_bytes, kb_inc, Tb, strategy, state); break;
        case MatrixLayout::Pr: mulConstant(1, kb_bytes, kb_inc, atype.packSize * Tb); break;
        default: stub();
    }

    incDecAddr(addrs, kb_bytes, kb_inc, 0, layout, strategy, state, problem.backward());

    state.ra.safeRelease(kb_bytes);
}

template <HW hw>
template <typename I>
void Generator<hw>::gemmBIncrement(const RegisterLayout &layout, const std::vector<GRFRange> &addrs, I kb_inc,
                                   const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int hb, int h)
{
        gemmBIncrementInternal(layout, addrs, kb_inc, problem, strategy, state, hb);
}

// B load for GEMM k loop.
template <HW hw>
void Generator<hw>::gemmBLoad(const GRFMultirange &regs, const RegisterLayout &layout, const std::vector<GRFRange> &addrs,
                              const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state)
{
    loadMatrix(regs, layout, addrs, strategy, state);
}

template <HW hw>
template <typename I>
void Generator<hw>::gemmBLoadInc(const GRFMultirange &regs, const RegisterLayout &layout, const std::vector<GRFRange> &addrs, I kb_inc,
                                 const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state)
{
    gemmBLoad(regs, layout, addrs, problem, strategy, state);
    gemmBIncrement(layout, addrs, kb_inc, problem, strategy, state);
}

template <HW hw>
template <bool doA>
void Generator<hw>::gemmAiBiRemLoadInc(int h, bool incremental, bool incrementalCopy, bool keepAddrTogether, bool willRemask, const Subregister &kSLMX,
                                       const GRFMultirange &Xi_regs, const RegisterLayout &Xi_layout, const vector<GRFRange> &Xi_addrs,
                                       const vector<RegisterLayout> &Xi_layoutK, const vector<vector<GRFRange>> &Xi_addrsK,
                                       const GRFMultirange &Xo_regs, const RegisterLayout &Xo_layout,
                                       const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state)
{
    auto kx_slm = doA ? state.ka_slm   : state.kb_slm;

    auto unrollKSLM = strategy.unrollKSLM;
    bool kSLMCountUp = state.kSLMCountUp;
    int kSLMSign = kSLMCountUp ? +1 : -1;
    auto kSLMPMod = kSLMCountUp ? ge : gt;

    bool prezero = !willRemask && ((doA ? state.slmASums : state.slmBSums)
                                       || (minOuterProductCount(hw, problem, strategy) > 1));

    if (!incremental) {
        if (prezero) zeroMatrix(Xi_regs, strategy);
        doA ? gemmALoad(Xi_regs, Xi_layout, Xi_addrs, problem, strategy, state)
            : gemmBLoad(Xi_regs, Xi_layout, Xi_addrs, problem, strategy, state);
    } else {
        bool simtCF = strategy.fused && (strategy.fusedLoop == (doA ? LoopN : LoopM));
        int simt = simtCF ? 16 : 1;
        Label done;

        keepAddrTogether &= (Xi_addrsK.size() > 1);

        kLoopModifiedFlagAP(state);
        cmp(simt | kSLMPMod | state.flagAP, kSLMX, 0);
        add(1, kSLMX, kSLMX, ((kx_slm > 1) ? 1 : unrollKSLM) * kSLMSign);

        if (prezero)
            zeroMatrix(incrementalCopy ? Xo_regs : Xi_regs, strategy);

        for (int hh = 0; hh < kx_slm; hh++) {
            int hhRem = kx_slm - hh - 1;

            Label skipInc;
            auto &skip = kSLMCountUp ? skipInc : done;

            simtCF ? goto12(16 | ~state.flagAP, skip)
                   :   jmpi(1  | ~state.flagAP, skip);

            auto nextCheck = [&] {
                if (hhRem > 0) {
                    cmp(simt | kSLMPMod | state.flagAP, kSLMX, 0);
                    add(1, kSLMX, kSLMX, ((hhRem == 1) ? (unrollKSLM - kx_slm + 1) : 1) * kSLMSign);
                }
            };

            if (!kSLMCountUp) nextCheck();

            int hh_eff = problem.backward() ? (kx_slm - 1 - hh) : hh;
            int hh_layout = hh_eff;
            int hh_addr   = hh_eff;

            if (Xi_layoutK.size() == 1) hh_layout = 0;
            if (Xi_addrsK.size()  == 1) hh_addr = 0;

            
            int kx_stride = unrollKSLM;
            if (strategy.kInterleave && (h % strategy.kInterleaveChunk) >= (strategy.kInterleaveChunk - unrollKSLM))
                kx_stride = unrollKSLM + strategy.kInterleaveChunk * (strategy.wg[LoopK] - 1);
            // OPTIMIZEME: delay inc if kx_slm = 1
            auto kx_inc = (Xi_addrsK.size() > 1) ? kx_stride :
                            ((hh + 1) != kx_slm) ? 1
                                                 : (kx_stride - kx_slm + 1);

            if (keepAddrTogether) kx_inc = 0;

            doA ? gemmALoad(Xi_regs, Xi_layoutK[hh_layout], Xi_addrsK[hh_addr], problem, strategy, state)
                : gemmBLoad(Xi_regs, Xi_layoutK[hh_layout], Xi_addrsK[hh_addr], problem, strategy, state);

            if (kSLMCountUp) {
                mark(skipInc);
                if (simtCF) join(16);
                nextCheck();
            }

            doA ? gemmAIncrement(Xi_layoutK[hh_layout], Xi_addrsK[hh_addr], kx_inc, problem, strategy, state)
                : gemmBIncrement(Xi_layoutK[hh_layout], Xi_addrsK[hh_addr], kx_inc, problem, strategy, state);

            if (incrementalCopy) {
                int rr_eff = doA ? 0 : hh_eff;
                int cc_eff = doA ? hh_eff : 0;
                copyRegisters(Xi_layoutK[hh_layout], Xo_layout, Xi_regs, Xo_regs, rr_eff, cc_eff, false, strategy, state);
            }
        }

        if (!kSLMCountUp) {
            mark(done);
            if (simtCF) join(16);
        }

        if (keepAddrTogether) {
            doA ? gemmAIncrement(Xi_layout, Xi_addrs, unrollKSLM, problem, strategy, state)
                : gemmBIncrement(Xi_layout, Xi_addrs, unrollKSLM, problem, strategy, state);
        }
    }
}

// Remask incoming global data for SLM copies.
template <HW hw>
void Generator<hw>::gemmSLMRemask(bool remaskA, bool remaskB, GRFMultirange &Ao_regs, GRFMultirange &Bo_regs, int kOffset, const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state)
{
    if (problem.backward()) stub();

    auto Ta = problem.Ta, Tb = problem.Tb;

    bool oremaskA = remaskA && (state.effCoopA == CoopSplit::K || state.effCoopA == CoopSplit::FullK);
    bool oremaskB = remaskB && (state.effCoopB == CoopSplit::K || state.effCoopB == CoopSplit::FullK);
    bool shareRemask = remaskA && remaskB && !oremaskA && !oremaskB && (Ta.bits() == Tb.bits());
    int aRemaskLen = state.ka_slm;
    int bRemaskLen = state.kb_slm;
    int iremaskA = 0, iremaskB = 1;
    if (shareRemask) iremaskB = iremaskA;

    Subregister offK_A, offK_B;
    if (oremaskA) {
        offK_A = state.ra.alloc_sub<uint32_t>();
        mulConstant(1, offK_A, state.lidN, state.ka_slm);
    }

    if (oremaskB) {
        offK_B = state.ra.alloc_sub<uint32_t>();
        mulConstant(1, offK_B, state.lidM, state.kb_slm);
    }

    if (shareRemask)
        aRemaskLen = bRemaskLen = std::max(aRemaskLen, bRemaskLen);

    if (remaskA) {
        setupTeardownRemask(Ta, iremaskA, true, aRemaskLen, state.K, strategy, state, kOffset, offK_A);
        remaskLayout(iremaskA, true, state.Ao_layout, Ao_regs, strategy, state);
        if (!shareRemask)
            setupTeardownRemask(Ta, iremaskA, false, aRemaskLen, state.K, strategy, state, kOffset, offK_A);
    }

    if (remaskB) {
        if (!shareRemask)
            setupTeardownRemask(Tb, iremaskB, true, bRemaskLen, state.K, strategy, state, kOffset, offK_B);
        remaskLayout(iremaskB, false, state.Bo_layout, Bo_regs, strategy, state);
        setupTeardownRemask(Tb, iremaskB, false, bRemaskLen, state.K, strategy, state, kOffset, offK_B);
    }

    state.ra.safeRelease(offK_A);
    state.ra.safeRelease(offK_B);
}

template <HW hw>
void Generator<hw>::kLoopAllocBarrierHeader(GEMMState &state)
{
    if (state.barrierHeader.isInvalid()) {
        state.barrierHeader = state.ra.alloc();
        state.barrierReady = false;
    }
}

template <HW hw>
GRF Generator<hw>::kLoopGetBarrierHeader(const GEMMStrategy &strategy, GEMMState &state)
{
    kLoopAllocBarrierHeader(state);
    if (!state.barrierReady) {
        if (state.r0_info.isARF()) stub();
        if (hw >= HW::XeHPG && strategy.activeThreads > 0)
            barrierheader(state.barrierHeader, strategy.activeThreads, GRF{state.r0_info.getBase()});
        else
            barrierheader(state.barrierHeader, GRF{state.r0_info.getBase()});
        state.barrierReady = true;
    }

    return state.barrierHeader;
}

// Activate or deactivate A/B remainders inside a k-loop.
template <HW hw>
void Generator<hw>::kLoopActivateABRemainder(bool active, bool doA, bool doB, const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int kOffset)
{
    bool &remActiveA = state.remActiveA, &remActiveB = state.remActiveB;
    auto &kMasksA = state.kMasksA, &kMasksB = state.kMasksB;
    auto ka_loadRem = state.ka_loadRem, kb_loadRem = state.kb_loadRem;

    bool a2D = isBlock2D(strategy.A.accessType);
    bool b2D = isBlock2D(strategy.B.accessType);
    bool ai2D = strategy.slmA && isBlock2D(state.Ai_strategy.accessType);
    bool bi2D = strategy.slmB && isBlock2D(state.Bi_strategy.accessType);

    // Update k masks and k remainder message descriptors as needed.
    Subregister rems[3] = {state.remainders[LoopM], state.remainders[LoopN], state.K};
    int offsets[3] = {0, 0, -kOffset};

    if (doA && active && remActiveA) {
        if (!kMasksA.empty())
            state.wipeActiveVFlags();
        loadMasks(kMasksA, rems, offsets, strategy, state);
        if (state.A_descRem)
            loadLoadStoreDescriptors(true, false, state.A_layoutRem[0], state.K, problem.A, strategy.A, strategy, state, true, kOffset);
    }
    if (doB && active && remActiveB) {
        if (!kMasksB.empty())
            state.wipeActiveVFlags();
        loadMasks(kMasksB, rems, offsets, strategy, state);
        if (state.B_descRem)
            loadLoadStoreDescriptors(true, false, state.B_layoutRem[0], state.K, problem.B, strategy.B, strategy, state, true, kOffset);
    }

    // Early exits if not changing between main loop and remainder loop.
    if (remActiveA == active) doA = false;
    if (remActiveB == active) doB = false;
    if (!active && ((doA && remActiveA) || (doB && remActiveB))) stub();
    if (!doA && !doB) return;

    if (doA) remActiveA = active;
    if (doB) remActiveB = active;

    // Prepare for descriptor-based remainders.
    if (state.A_descRem && state.B_descRem)
        stub();
    else if (state.A_descRem)
        setupTeardownLoadStoreDesc(true, state.A_layoutRem, strategy, state);
    else if (state.B_descRem)
        setupTeardownLoadStoreDesc(true, state.B_layoutRem, strategy, state);

    // Adjust A/B/Ai/Bi addresses if needed.
    if (doA) adjustSubblockAddrs(state.A_layoutRem, state.A_addrsRem, state.A_layout, state.A_addrs, strategy, state);
    if (doB) adjustSubblockAddrs(state.B_layoutRem, state.B_addrsRem, state.B_layout, state.B_addrs, strategy, state);

    if (doA && strategy.slmA && (state.effCoopA == CoopSplit::K) && !ai2D) {
        vector<GRFRange> tempAddrs;
        auto tempLayout = state.Ai_layout.slice(tempAddrs, state.Ai_addrs, true, 0, 1, state.Ai_strategy.padded);
        adjustSubblockAddrs(tempLayout, tempAddrs, state.Ai_layout, state.Ai_addrs, strategy, state);
    }
    if (doB && strategy.slmB && (state.effCoopB == CoopSplit::K) && !bi2D) {
        vector<GRFRange> tempAddrs;
        auto tempLayout = state.Bi_layout.slice(tempAddrs, state.Bi_addrs, false, 0, 1, state.Bi_strategy.padded);
        adjustSubblockAddrs(tempLayout, tempAddrs, state.Bi_layout, state.Bi_addrs, strategy, state);
    }

    if (doA && a2D && (ka_loadRem > 1))
        setAddrRemainder(state.A_addrsRem, state.A_layoutRem, Subregister(), state.K, strategy, state);
    if (doB && b2D && (kb_loadRem > 1))
        setAddrRemainder(state.B_addrsRem, state.B_layoutRem, state.K, Subregister(), strategy, state);

    // Start using k masks/descriptors if needed.
    if (doA && state.A_lateKRem && !strategy.A.padded) {
        if (!state.A_descRem) {
            state.A_layoutRem = state.A_layout;
            state.A_addrsRem = state.A_addrs;
        }
        auto remOpts = state.A_descRem ? AllowDescriptors : AvoidFragment;
        addRemainder(state.A_layoutRem, state.A_addrsRem, state.inputs.lda, false, true, remOpts, strategy, state);
        if (!assignMasks(state.A_layoutRem, LoopM, LoopK, kMasksA, strategy, state, true, &state.AB_masks)) stub();
        if (state.A_descRem) {
            loadLoadStoreDescriptors(true, false, state.A_layoutRem[0], state.K, problem.A, strategy.A, strategy, state, true, kOffset);
            if (!state.A_layoutRem.assignAllDescs()) stub();
        }
    }
    if (doB && state.B_lateKRem && !strategy.B.padded) {
        if (!state.B_descRem) {
            state.B_layoutRem = state.B_layout;
            state.B_addrsRem = state.B_addrs;
        }
        auto remOpts = state.B_descRem ? AllowDescriptors : AvoidFragment;
        addRemainder(state.B_layoutRem, state.B_addrsRem, state.inputs.ldb, true, false, remOpts, strategy, state);
        if (!assignMasks(state.B_layoutRem, LoopK, LoopN, kMasksB, strategy, state, true, &state.AB_masks)) stub();
        if (state.B_descRem) {
            loadLoadStoreDescriptors(true, false, state.B_layoutRem[0], state.K, problem.B, strategy.B, strategy, state, true, kOffset);
            if (!state.B_layoutRem.assignAllDescs()) stub();
        }
    }

    if (problem.backward()) {
        if (doA) for (auto &mask: kMasksA)
            mask.reverse(ka_loadRem);
        if (doB) for (auto &mask: kMasksB)
            mask.reverse(kb_loadRem);
    }

    if (doA) loadMasks(kMasksA, rems, offsets, strategy, state);
    if (doB) loadMasks(kMasksB, rems, offsets, strategy, state);

    // Recalculate ld increments if needed.
    gemmCalcIncrements(problem, strategy, state, ka_loadRem, kb_loadRem, doA, doB);
}

// Activate or deactivate SLM remainders inside a k-loop.
template <HW hw>
void Generator<hw>::kLoopActivateSLMRemainder(bool active, bool preactivate, const GEMMProblem &problem, const GEMMStrategy &strategy, GEMMState &state, int kOffset)
{
    auto Ta_ext = problem.Ta_ext, Tb_ext = problem.Tb_ext;

    bool slmA = strategy.slmA, slmB = strategy.slmB;
    auto unrollKSLM = strategy.unrollKSLM;
    auto &kSLMA = state.kSLMA, &kSLMB = state.kSLMB;
    auto &kSLMStorage = state.kSLMStorage;
    bool &remActiveSLM = state.remActiveSLM;
    bool &slmRemaskA = state.slmRemaskA, &slmRemaskB = state.slmRemaskB;

    bool Ai_incrementalRem = state.Ai_incrementalRem;
    bool Bi_incrementalRem = state.Bi_incrementalRem;
    bool Ai_remIncrCopy = state.Ai_remIncrCopy;
    bool Bi_remIncrCopy = state.Bi_remIncrCopy;
    bool Ai_lateKRem = state.Ai_lateKRem;
    bool Bi_lateKRem = state.Bi_lateKRem;

    bool needKSLMAMask = (Ai_lateKRem && (state.effCoopA == CoopSplit::K || state.effCoopA == CoopSplit::FullK));
    bool needKSLMBMask = (Bi_lateKRem && (state.effCoopB == CoopSplit::K || state.effCoopB == CoopSplit::FullK));
    bool needKSLMA = Ai_incrementalRem || needKSLMAMask;
    bool needKSLMB = Bi_incrementalRem || needKSLMBMask;

    bool shareKMasksAiBi = !(needKSLMAMask || needKSLMBMask);
    auto &kMasksAi = state.kMasksAi;
    auto &kMasksBi = state.kMasksBi;
    auto &effKMasksBi = shareKMasksAiBi ? state.kMasksAi : state.kMasksBi;
    auto &initSLMKOffset = state.initSLMKOffset;

    auto minOPCount = minOuterProductCount(hw, problem, strategy);

    // Calculate or recalculate SLM k remainders as needed.
    if (active && !preactivate && kSLMStorage.isInvalid()) {
        if (needKSLMA || needKSLMB)
            kSLMStorage = state.ra.alloc_sub<uint32_t>();

        if (needKSLMA && !preactivate) {
            kSLMA = kSLMStorage.w(0);
            gemmCalcKSLMA(problem, strategy, state);
        }

        if (needKSLMB && !preactivate) {
            kSLMB = kSLMStorage.w(1);
            gemmCalcKSLMB(problem, strategy, state);
        }

        if ((needKSLMA || needKSLMB) && kOffset != 0) {
                add(2 | sat, kSLMStorage.uw()(1), kSLMStorage.uw()(1), kOffset);
        }

        initSLMKOffset = kOffset;
    }

    // k mask information.
    Subregister remsAi[3] = {state.remaindersCoop[LoopM], state.remaindersCoop[LoopN], state.K};
    Subregister remsBi[3] = {state.remaindersCoop[LoopM], state.remaindersCoop[LoopN], state.K};
    int offsetsAi[3] = {0, 0, -kOffset};
    int offsetsBi[3] = {0, 0, -kOffset};

    if (needKSLMAMask) {
        remsAi[2] = kSLMA;
        offsetsAi[2] += initSLMKOffset;
    }
    if (needKSLMBMask) {
        remsBi[2] = kSLMB;
        offsetsBi[2] += initSLMKOffset;
    }

    // If not changing between main loop and remainder, update k masks as needed and return.
    if (remActiveSLM == active) {
        if (active && !preactivate) {
            if (!kMasksAi.empty() || !kMasksBi.empty())
                state.wipeActiveVFlags();
            loadMasks(kMasksAi, remsAi, offsetsAi, strategy, state);
            loadMasks(kMasksBi, remsBi, offsetsBi, strategy, state);
        }
        return;
    }

    // Not possible to deactivate remainder path with late k remainder.
    if (!active && remActiveSLM && (Ai_lateKRem || Bi_lateKRem)) stub();
    remActiveSLM = active;

    // Start using k masks if needed.
    if (Ai_lateKRem && !state.Ai_strategy.padded) {
        state.Ai_layoutRem = state.Ai_layout;
        state.Ai_addrsRem = state.Ai_addrs;
        addRemainder(state.Ai_layoutRem, state.Ai_addrsRem, state.inputs.lda, false, true, AvoidFragment, strategy, state, state.Ai_regCount);
        if (!assignMasks(state.Ai_layoutRem, LoopM, LoopK, kMasksAi, strategy, state, true, &state.AB_masksCoop)) stub();
        if (state.aioShare && state.Ao_regsRem.empty() && state.Ai_layoutRem[0].crosspack != state.Ai_layout[0].crosspack) {
            state.aioShareRem = false;
            state.Ao_regsRem = state.ra.allocRange(state.Ao_layout.regs());
        }
    }
    if (Bi_lateKRem && !state.Bi_strategy.padded) {
        state.Bi_layoutRem = state.Bi_layout;
        state.Bi_addrsRem = state.Bi_addrs;
        addRemainder(state.Bi_layoutRem, state.Bi_addrsRem, state.inputs.ldb, true, false, AvoidFragment, strategy, state, state.Bi_regCount);
        if (!assignMasks(state.Bi_layoutRem, LoopK, LoopN, effKMasksBi, strategy, state, true, &state.AB_masksCoop)) stub();
        if (state.bioShare && state.Bo_regsRem.empty() && state.Bi_layoutRem[0].crosspack != state.Bi_layout[0].crosspack) {
            state.bioShareRem = false;
            state.Bo_regsRem = state.ra.allocRange(state.Bo_layout.regs());
        }
    }

    if (problem.backward()) {
        for (auto &mask: kMasksAi)
            mask.reverse(unrollKSLM);
        for (auto &mask: kMasksBi)
            mask.reverse(unrollKSLM);
    }

    if (!preactivate) {
        loadMasks(kMasksAi, remsAi, offsetsAi, strategy, state);
        loadMasks(kMasksBi, remsBi, offsetsBi, strategy, state);
    }

    bool mayAccessAllK = (minOPCount > 1) || problem.needsASums() || problem.needsBSums();
    bool asIfMaskedAi = Ai_lateKRem && state.Ai_strategy.padded;
    bool asIfMaskedBi = Bi_lateKRem && state.Bi_strategy.padded;
    slmRemaskA = slmA && mayAccessAllK && !Ai_remIncrCopy && needsRemask(Ta_ext, true,  state.Ai_layoutRem, state.Ai, state.Ai_strategy, asIfMaskedAi);
    slmRemaskB = slmB && mayAccessAllK && !Bi_remIncrCopy && needsRemask(Tb_ext, false, state.Bi_layoutRem, state.Bi, state.Bi_strategy, asIfMaskedBi);
}

GEMMSTONE_NAMESPACE_END
