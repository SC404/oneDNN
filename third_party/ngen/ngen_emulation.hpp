/*******************************************************************************
* Copyright 2020-2025 Intel Corporation
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

#ifndef NGEN_EMULATION_HPP
#define NGEN_EMULATION_HPP

#include "ngen_config_internal.hpp"

#ifdef NGEN_ENABLE_SOURCE_LOCATION
#include <source_location>
#endif

#include <exception>

namespace NGEN_NAMESPACE {

struct EmulationStrategy {
    // Emulate 64-bit arithmetic (required for GenXLP)
    bool emulate64 = false;
    // Emulate DW x DW -> DW multiplication (required for Gen12)
    bool emulateDWxDW = false;
    // Use 32-bit adds for 64-bit arithmetic, assuming no 2^32 boundaries crossed.
    bool emulate64_add32 = false;
    // Emulate DW x DW -> QW multiplication (XeHPC)
    bool emulate64_mul = false;
    // Emulate QW and/or/xor operations (XeHPC)
    bool emulate64_logic = false;
    // Don't emulate QW shl/shr (XeHPC)
    bool noemulate64_shift = false;

    EmulationStrategy() = default;
    EmulationStrategy(HW hw_, int stepping = 0) {
        if (hw_ == HW::Gen11) emulate64 = true;
        if (hw_ >= HW::Gen11) emulateDWxDW = true;
        if (hw_ == HW::Gen12LP) emulate64 = true;
        if (hw_ == HW::XeHPG) emulate64 = true;
        if (hw_ >= HW::XeHPC) {
            if (hw_ == HW::XeHPC && stepping < SteppingPVCXTB0)
                emulate64 = noemulate64_shift = true;
            else
                emulate64_mul = emulate64_logic = true;
        }
        emulate64_mul |= emulate64;
    }
};

struct EmulationState {
    GRF temp[2];                 // Temporary GRFs for use in emulation sequences
    FlagRegister flag;           // Flag register for use in emulating 64-bit adds (optional, avoids temporary registers/acc)
    int flagOffset = 0;                // Channel offset to use with flag register.
};

// Implementation wrapped as static methods in non-instantiated class.
// Clients should declare EmulationImplementation as a friend.
struct EmulationImplementation {
#ifdef NGEN_ENABLE_SOURCE_LOCATION
    [[noreturn]] static void stub(std::source_location where = std::source_location::current()) {
        throw std::runtime_error(std::string("Unimplemented (at ")
                + std::string(where.file_name()) + ":"
                + std::to_string(where.line()) + ")");
    }
#else
    [[noreturn]] static void stub() {
        throw std::runtime_error("Unimplemented");
    }
#endif

    template <typename DT, typename O>
    static void applyDefaultType(O &op)
    {
        if (op.getType() == DataType::invalid)
            op.setType(getDataType<DT>());
    }

    template <typename O>
    static bool isQW(const O &op)
    {
        return op.getType()== DataType::q || op.getType() == DataType::uq;
    }

    template <typename O>
    static bool isDW(const O &op)
    {
        return op.getType() == DataType::d || op.getType() == DataType::ud;
    }

    template <typename O>
    static bool isW(const O &op) {
        return op.getType() == DataType::w || op.getType() == DataType::uw;
    }

    static bool isDW(const Immediate &op)
    {
        if (op.getType() == DataType::w)
            return int16_t(static_cast<uint64_t>(op)) < 0;
        else
            return op.getType() == DataType::d || op.getType() == DataType::ud;
    }

    template <typename O> static O expandDW(const O &op) { return op; }
    static Immediate expandDW(const Immediate &op) { return op.forceInt32(); }

    template <typename T1, typename T2> static bool equal(const T1 &o1, const T2 &o2) { return o1 == o2; }
    static bool equal(const RegData &o1, const Immediate &o2) { return false; }

    static void downgradeToDW(RegData &op)
    {
        if (isQW(op)) {
            op.setType((op.getType() == DataType::q) ? DataType::d : DataType::ud);
            op.setOffset(op.getOffset() * 2);
        }
    }

    static void downgradeToDW(Immediate &op)
    {
        if (isQW(op))
            op.setType((op.getType() == DataType::q) ? DataType::d : DataType::ud);
    }

    // Get the DW equivalent of a QW region.
    static void makeDWPair(RegData &op, int esize)
    {
        if (isQW(op)) {
            downgradeToDW(op);
            if (op.getHS() > 1) {
                if (op.getVS() != op.getHS() * op.getWidth()) stub();
                op.setRegion(op.getHS() * 2, 2, 1);
            } else {
                auto newVS = op.getVS() * 2;
                if (esize == op.getWidth())
                    newVS = esize * 2;
                op.setRegion(newVS, op.getWidth() * 2, 1);
            }
        }
    }

    // Split a register into DW pairs.
    static void splitToDW(RegData in, RegData &outLo, RegData &outHi)
    {
        bool isQ = (in.getType() == DataType::q);
        bool isUQ = (in.getType() == DataType::uq);

        if (isQ || isUQ) {
            outLo = in;
            outLo.setRegion(in.getVS() * 2, in.getWidth(), in.getHS() * 2);
            outLo.setOffset(in.getOffset() * 2);
            outLo.setType(DataType::ud);

            outHi = outLo;
            outHi.setOffset(in.getOffset() * 2 + 1);
            outHi.setType(isQ ? DataType::d : DataType::ud);
        } else {
            outLo = in;
            outHi = Subregister{};      // invalid
        }
    }

    // Split an Immediate into DW pairs.
    static void splitToDW(const Immediate &in, Immediate &outLo, Immediate &outHi)
    {
        bool isQ = (in.getType() == DataType::q);
        bool isUQ = (in.getType() == DataType::uq);

        if (isQ || isUQ) {
            outLo = uint32_t(static_cast<uint64_t>(in));
            outLo = outLo.forceInt32();
            outLo.setType(DataType::ud);

            outHi = uint32_t(static_cast<uint64_t>(in) >> 32);
            outHi = outHi.forceInt32();
            outHi.setType(isQ ? DataType::d : DataType::ud);
        } else {
            outLo = in;
            outHi = uint16_t(0);
        }
    }

    static RegData lowWord(RegData in)
    {
        if (isW(in)) return in;

        auto outLo = in;
        outLo.setRegion(in.getVS() * 2, in.getWidth(), in.getHS() * 2);
        outLo.setOffset(in.getOffset() * 2);
        outLo.setType(DataType::uw);

        return outLo;
    }

    static Immediate lowWord(const Immediate &in)
    {
        return uint16_t(static_cast<uint64_t>(in) & 0xffff);
    }

    static RegData highWord(RegData in)
    {
        auto out = lowWord(in);
        out.setOffset(out.getOffset() + 1);
        return out;
    }

    static Immediate highWord(const Immediate &in)
    {
        return uint16_t(static_cast<uint64_t>(in) >> 16);
    }

    static bool isUnitStride(const RegData &rd)
    {
        return (rd.getHS() == 1 && rd.getVS() == rd.getWidth());
    }

    static void regionVSAdvance(HW hw, RegData &rd, int i)
    {
        int ne = GRF::bytes(hw) / rd.getBytes();
        int advance = rd.getWidth() > 0 ? (i / rd.getWidth()) * rd.getVS()
                                        : i * rd.getHS();
        int noffset = rd.getOffset() + advance;
        if (noffset >= ne) {
            noffset -= ne;
            rd.setBase(rd.getBase() + 1);
        }
        rd.setOffset(noffset);
    }

    static void regionVSAdvance(HW hw, Immediate &imm, int i) {}

    // Move, emulating 64-bit moves with 32-bit (generally a good idea).
    template <typename DT = void, typename Generator>
    static void emov(Generator &g, const InstructionModifier &mod, RegData dst, RegData src0, const EmulationStrategy &strategy, SourceLocation loc = {})
    {
        applyDefaultType<DT>(dst);
        applyDefaultType<DT>(src0);

        bool dstQ = isQW(dst);
        bool s0Q = isQW(src0);
        bool s0D = isDW(src0);
        bool s0W = isW(src0);
        bool s0B = isW(src0);
        bool isDF = (src0.getType() == DataType::df && dst.getType() == DataType::df);
        bool unaligned = (mod.getExecSize() > 1 && src0.getHS() != 0 && src0.getOffset() != dst.getOffset());

        if ((dstQ && (s0D || s0W || s0B)) && strategy.emulate64) {
            if (src0.getNeg()) stub();
            bool s0Signed = isSigned(src0.getType());
            RegData dstHi, dstLo;
            splitToDW(dst, dstLo, dstHi);
            g.mov(mod, dstLo, src0, loc);
            if (!s0Signed)
                g.mov(mod, dstHi, 0, loc);
            else
                g.asr(mod, dstHi, dstLo, uint16_t(31), loc);
        } else if (((dstQ || s0Q) && strategy.emulate64)
                || (isDF && unaligned && g.getHardware() >= HW::XeHP)) {
            if (dstQ != s0Q) stub();

            auto mod2x = mod;
            mod2x.setExecSize(mod.getExecSize() * 2);

            makeDWPair(dst, mod.getExecSize());
            makeDWPair(src0, mod.getExecSize());
            g.mov(mod2x, dst, src0, loc);
        } else if (dst.getType() == DataType::f && src0.getType() == DataType::bf && (src0.getHS() != 1 || mod.getExecSize() == 1)) {
            // Emulate bf16->f32 upconversion
            dst.setType(DataType::ud);
            src0.setType(DataType::uw);
            g.shl(mod, dst, src0, 16, loc);
        } else if (src0.getType() == DataType::bf8
                && dst.getType() == DataType::f) {
            RegData hfTmp = src0;
            hfTmp.setType(DataType::uw);
            g.shl(mod, hfTmp, src0.setType(DataType::ub), 8, loc);
            g.mov(mod, dst, hfTmp.setType(DataType::hf), loc);
        } else
            g.mov(mod, dst, src0, loc);
    }

    template <typename DT = void, typename Generator>
    static void emov(Generator &g, const InstructionModifier &mod, RegData dst, Immediate src0, const EmulationStrategy &strategy, SourceLocation loc = {})
    {
        applyDefaultType<DT>(dst);
        applyDefaultType<DT>(src0);

        bool dstQ = isQW(dst);
        bool s0Q = isQW(src0);

        if ((dstQ || s0Q) && strategy.emulate64) {
            if (!dstQ) stub();

            RegData dstHi, dstLo;
            Immediate s0Hi = 0, s0Lo = 0;

            splitToDW(src0, s0Lo, s0Hi);

            if (static_cast<uint64_t>(s0Lo) == static_cast<uint64_t>(s0Hi) && dst.getHS() <= 1) {
                auto mod2x = mod;
                mod2x.setExecSize(mod.getExecSize() * 2);

                downgradeToDW(dst);
                dst.setRegion(0, 0, 1);
                g.mov(mod2x, dst, s0Lo, loc);
            } else {
                splitToDW(dst, dstLo, dstHi);
                g.mov(mod, dstLo, s0Lo, loc);
                g.mov(mod, dstHi, s0Hi, loc);
            }
        } else
            g.mov(mod, dst, src0, loc);
    }

    template <typename Generator>
    static void eaddSignExtend1(Generator &g, const InstructionModifier &mod, bool &doSub, const Immediate &src1, Immediate &s1LoPos, const Immediate &s1Lo, const Immediate &s1Hi, bool &s1Q, const GRF (&temp)[2], const SourceLocation & loc)
    {
        uint64_t raw = static_cast<uint64_t>(src1);
        if (src1.getType() == DataType::d) {
            auto val = int32_t(raw);
            s1LoPos = uint32_t(std::abs(val));
            doSub = (val < 0);
        } else if (src1.getType() == DataType::w) {
            auto val = int16_t(raw);
            s1LoPos = uint16_t(std::abs(val));
            doSub = (val < 0);
        }
    }

    template <typename Generator>
    static void eaddSignExtend1(Generator &g, const InstructionModifier &mod, bool &doSub, const RegData &src1, RegData &s1LoPos, RegData &s1Lo, RegData &s1Hi, bool &s1Q, const GRF (&temp)[2], const SourceLocation &loc)
    {
        s1Q = true;
        s1Hi = temp[0].d();
        if (s1Lo.getNeg()) {
            g.asr(mod, s1Hi, -s1Lo, uint16_t(31), loc);
            s1Hi = -s1Hi;
        } else
            g.asr(mod, s1Hi, s1Lo, uint16_t(31), loc);
        s1Lo.setType(DataType::ud);
    }

    static void eaddHandleS1Neg(bool &doSub, RegData &s1LoPos, const RegData &s1Lo)
    {
        if (isSigned(s1Lo.getType())) stub();
        doSub = s1Lo.getNeg();
        s1LoPos = -s1Lo;
    }

    static void eaddHandleS1Neg(bool &doSub, const Immediate &s1LoPos, const Immediate &s1Lo)
    {
        /* no-op */
    }

    template <typename Generator>
    static void eaddFixupQD(Generator &g, const InstructionModifier &mod, const FlagRegister &flag, const RegData &dstHi, const RegData &src1, const SourceLocation &loc)
    {
        if ((src1.getBytes() < 8) && isSigned(src1.getType())) {
            // Add sign extension of src1 to high 32 bits of dst (inefficient but rarely used path).
            g.cmp(mod | (src1.getNeg() ? g.le : g.lt) | flag, src1, 0, loc);
            g.add(mod | flag, dstHi, dstHi, -1, loc);
        }
    }

    template <typename Generator>
    static void eaddFixupQD(Generator &g, const InstructionModifier &mod, const FlagRegister &flag, const RegData &dstHi, const Immediate &src1, const SourceLocation &loc) {
        /* no-op */
    }

    static bool eaddIsNegative(const RegData &r)
    {
        return r.getNeg();
    }

    static bool eaddIsNegative(const Immediate &i)
    {
        return int32_t(uint64_t(i)) < 0;
    }

    // Integer addition, emulating 64-bit arithmetic if configured.
    template <typename DT = void, typename S1, typename Generator>
    static void eaddInternal(Generator &g, const InstructionModifier &mod, RegData dst, RegData src0, S1 src1, const EmulationStrategy &strategy, const EmulationState &state, const SourceLocation &loc)
    {
        const auto &temp = state.temp;

        applyDefaultType<DT>(dst);
        applyDefaultType<DT>(src0);
        applyDefaultType<DT>(src1);

        bool dstQ = isQW(dst);
        bool s0Q = isQW(src0);
        bool s1Q = isQW(src1);

        if (dstQ && strategy.emulate64_add32) {
            RegData dstHi, dstLo, s0Hi, s0Lo;
            S1 s1Hi, s1Lo;

            splitToDW(dst, dstLo, dstHi);
            splitToDW(src0, s0Lo, s0Hi);
            splitToDW(src1, s1Lo, s1Hi);
            g.add(mod, dstLo, s0Lo, s1Lo, loc);

            if (s0Q && s1Q) {
                if (!equal(dstHi, s0Hi) && !equal(dstHi, s1Hi))
                    g.add(mod, dstHi, s0Hi, s1Hi, loc);
            } else if (s0Q) {
                if (!equal(dstHi, s0Hi)) g.mov(mod, dstHi, s0Hi, loc);
            } else if (s1Q) {
                if (!equal(dstHi, s1Hi)) g.mov(mod, dstHi, s1Hi, loc);
            } else
                g.mov(mod, dstHi, uint16_t(0), loc);
        } else if (!strategy.emulate64)
            g.add(mod, dst, src0, src1, loc);
        else {
            if (!dstQ) {
                downgradeToDW(src0);
                downgradeToDW(src1);
                g.add(mod, dst, src0, src1, loc);
            } else {
                RegData dstHi, dstLo, s0Hi, s0Lo;
                S1 s1Hi, s1Lo, s1LoPos;
                FlagRegister flag = state.flag;

                splitToDW(dst, dstLo, dstHi);
                splitToDW(src0, s0Lo, s0Hi);
                splitToDW(src1, s1Lo, s1Hi);
                s1LoPos = s1Lo;

                bool s0Signed = isSigned(s0Lo.getType());
                bool s1Signed = isSigned(s1Lo.getType());

                if (flag.isValid() && !eaddIsNegative(s0Lo)) {
                    // Use flag register + ov.
                    auto Mx = g.ExecutionOffset(state.flagOffset);
                    bool neg = eaddIsNegative(s1Lo);
                    bool revFlag = false;

                    auto s0LoUD = s0Lo;
                    auto s1LoMod = s1Lo;
                    s0LoUD.setType(DataType::ud);
                    if (s1Signed && !std::is_base_of<Immediate, S1>::value) {
                        s1LoMod.setType(DataType::ud);
                        revFlag = neg;
                        neg = false;
                    }

                    g.add(mod | Mx | g.ov | flag, dstLo, s0LoUD, s1LoMod, loc);
                    if (s0Q && s1Q)
                        g.add(mod, dstHi, s0Hi, s1Hi, loc);
                    else if (s0Q && !equal(dstHi, s0Hi))
                        g.mov(mod, dstHi, s0Hi, loc);
                    else if (s1Q && !equal(dstHi, s1Hi))
                        g.mov(mod, dstHi, s1Hi, loc);
                    else if (!s0Q && !s1Q)
                        g.mov(mod, dstHi, 0, loc);
                    g.add(mod | Mx | (revFlag ? ~flag : flag), dstHi, dstHi, neg ? -1 : +1, loc);
                    eaddFixupQD(g, mod | Mx, flag, dstHi, src0, loc);
                    eaddFixupQD(g, mod | Mx, flag, dstHi, src1, loc);
                } else {
                    // Slow path: addc/subb + acc.
                    RegData carry = temp[0].ud();
                    bool lateCarry = false;
                    RegData subDstLo;
                    bool doSub = false;

                    // For :uq + :d or :q + :ud, sign extend 32-bit input to 64 bits.
                    if (s0Signed != s1Signed) {
                        if (s0Signed) {
                            s0Q = true;
                            s0Hi = temp[0].d();
                            g.asr(mod, s0Hi, s0Lo, uint16_t(31), loc);
                            s0Lo.setType(DataType::ud);
                            if (s0Lo.getNeg())
                                s0Hi = -s0Hi;
                        } else
                            eaddSignExtend1(g, mod, doSub, src1, s1LoPos, s1Lo, s1Hi, s1Q, temp, loc);
                        carry = temp[1].ud();
                        lateCarry = true;
                    }

                    // Handle modifiers.
                    if (s0Lo.getNeg()) stub();
                    eaddHandleS1Neg(doSub, s1LoPos, s1Lo);

                    // Compute low 32 bits, saving carry/borrow.
                    if (dstLo.getOffset() != 0) {
                        doSub ? g.subb(mod, g.null.retype(s0Lo.getType()), s0Lo, s1LoPos, loc)
                              : g.addc(mod, g.null.retype(s0Lo.getType()), s0Lo, s1Lo, loc);
                        g.add(mod, dstLo, s0Lo, s1Lo, loc);
                    } else if ((mod.getExecSize() > 1) && !isUnitStride(dstLo)) {
                        subDstLo = temp[1].ud();
                        doSub ? g.subb(mod, subDstLo, s0Lo, s1LoPos, loc)
                              : g.addc(mod, subDstLo, s0Lo, s1Lo, loc);
                    } else {
                        doSub ? g.subb(mod, dstLo, s0Lo, s1LoPos, loc)
                              : g.addc(mod, dstLo, s0Lo, s1Lo, loc);
                    }

                    // Retrieve carry from accumulator, unless it conflicts with subDstLo.
                    if (!lateCarry) g.mov(mod, carry, g.acc0.ud(), loc);

                    // Move low 32-bits to final resting place, if needed.
                    if (subDstLo.isValid()) g.mov(mod, dstLo, subDstLo, loc);

                    // Retrieve carry from accumulator once subDstLo isn't needed.
                    if (lateCarry) g.mov(mod, carry, g.acc0.ud(), loc);

                    if (doSub)
                        carry = -carry;

                    // Compute high 32 bits of sum.
                    if (s0Q && s1Q) {
                        g.add(mod, dstHi, s0Hi, s1Hi, loc);
                        g.add(mod, dstHi, carry, dstHi, loc);
                    } else if (s0Q)
                        g.add(mod, dstHi, carry, s0Hi, loc);
                    else if (s1Q)
                        g.add(mod, dstHi, carry, s1Hi, loc);
                    else
                        g.mov(mod, dstHi, carry, loc);
                }
            }
        }
    }

    template <typename DT = void, typename Generator>
    static void eadd(Generator &g, const InstructionModifier &mod, const RegData &dst, const RegData &src0, const RegData &src1, const EmulationStrategy &strategy, const EmulationState &state, SourceLocation loc = {}) {
        if (src0.getNeg() && !src1.getNeg() && strategy.emulate64 && !strategy.emulate64_add32)
            eaddInternal<DT>(g, mod, dst, src1, src0, strategy, state, loc);
        else
            eaddInternal<DT>(g, mod, dst, src0, src1, strategy, state, loc);
    }

    template <typename DT = void, typename Generator>
    static void eadd(Generator &g, const InstructionModifier &mod, const RegData &dst, const RegData &src0, Immediate src1, const EmulationStrategy &strategy, const EmulationState &state, SourceLocation loc = {}) {
        eaddInternal<DT>(g, mod, dst, src0, src1, strategy, state, loc);
    }

    // Compatibility shim to work around missing if constexpr support in C++11/14
    template <typename Generator>
    static void emulInternal(Generator &g, const InstructionModifier &mod, RegData dst, Immediate src0, RegData src1, const EmulationStrategy &strategy, const EmulationState &state, const SourceLocation &loc) {
        stub();
    }

    // Integer multiplication, emulating 32x32 multiplication as configured.
    template <typename DT = void, typename S1, typename Generator>
    static void emulInternal(Generator &g, const InstructionModifier &mod, RegData dst, RegData src0, S1 src1, const EmulationStrategy &strategy, const EmulationState &state, const SourceLocation &loc) {
        applyDefaultType<DT>(dst);
        applyDefaultType<DT>(src0);
        applyDefaultType<DT>(src1);

        bool dstD = isDW(dst);
        bool dstQ = isQW(dst);
        bool s0W = isW(src0);
        bool s0D = isDW(src0);
        bool s0Q = isQW(src0);
        bool s1W = isW(src1);
        bool s1D = isDW(src1);
        bool s1Q = isQW(src1);
        const bool s1Immed = std::is_base_of<Immediate, S1>::value;

        bool s0Signed = isSigned(src0.getType());
        bool s1Signed = isSigned(src1.getType());
        auto mulHiType = (s0Signed || s1Signed) ? DataType::d : DataType::ud;

        bool emulate64 = strategy.emulate64_mul;

        if (s0Q) {
            if (!dstQ) stub();

            auto dstDWType = s1Signed ? DataType::d : DataType::ud;
            RegData dstLo, dstHi;
            RegData s0Hi, s0Lo;
            splitToDW(dst, dstLo, dstHi);
            splitToDW(src0, s0Lo, s0Hi);
            dstLo.setType(dstDWType);
            dstHi.setType(dstDWType);
            auto accLo
                = g.acc0.retype(dstDWType)[dstLo.getOffset()](dstLo.getHS());
            auto accHi
                = g.acc0.retype(dstDWType)[dstHi.getOffset()](dstHi.getHS());

            if(s1W) {
                g.mul(mod, accLo, s0Lo, src1, loc);
                g.mach(mod, dstLo, s0Lo, 0, loc);
                g.mad(mod, dstHi, dstLo, s0Hi, src1, loc);
                g.mov(mod, dstLo, accLo, loc);
            } else if(s1D) {
                auto s1Lo = lowWord(src1);
                g.mul(mod, accLo, s0Lo, s1Lo, loc);
                g.mach(mod, dstLo, s0Lo, src1, loc);
                g.mul(mod, accHi, s0Hi, s1Lo, loc);
                g.macl(mod, dstHi, s0Hi, src1, loc);
                g.add(mod, dstHi, dstHi, dstLo, loc);
                g.mov(mod, dstLo, accLo, loc);
            } else stub();
        } else if (s1Q) {
            if(!s1Immed) {
                return emulInternal(g, mod, dst, src1, src0, strategy, state, loc);
            }

            if (!s0D || !dstQ) stub();
            auto s0Type = src0.getType();
            RegData dstLo, dstHi;
            S1 s1Hi, s1Lo;
            splitToDW(dst, dstLo, dstHi);
            splitToDW(src1, s1Lo, s1Hi);
            s1Hi = expandDW(s1Hi);
            s1Lo = expandDW(s1Lo);
            dstLo.setType(src0.getType());
            dstHi.setType(src0.getType());
            auto s1W0 = lowWord(s1Lo);
            auto s1W2 = lowWord(s1Hi);
            auto accLo
                    = g.acc0.retype(s0Type)[dstLo.getOffset()](dstLo.getHS());
            auto accHi
                    = g.acc0.retype(s0Type)[dstHi.getOffset()](dstHi.getHS());
            g.mul(mod, accHi, src0, s1W2, loc);
            g.macl(mod, dstHi, src0, s1Hi, loc);
            g.mul(mod, accLo, src0, s1W0, loc);
            g.mach(mod, dstLo, src0, s1Lo, loc);
            g.add(mod, dstHi, dstHi, dstLo, loc);
            g.mov(mod, dstLo, accLo, loc);
        } else if (dstQ && s0W && s1W) {
            RegData dstLo, dstHi;
            splitToDW(dst, dstLo, dstHi);

            g.mul(mod, dstLo, src0, src1, loc);

            dstHi.setType(mulHiType);
            dstLo.setType(mulHiType);

            if (s0Signed || s1Signed)
                g.asr(mod, dstHi, dstLo, 31, loc);
            else
                g.mov(mod, dstHi, 0, loc);
        } else if (dstQ && s0W && s1D) {
            stub();
        } else if (dstQ && s0D && s1W && !s1Immed && !emulate64 && !strategy.emulateDWxDW) {
            auto acc = g.acc0.d();
            g.mov(mod, acc, src1, loc);
            g.mul(mod, dst, acc, src0, loc);
        } else if (dstQ && s0D && ((s1W && !s1Immed) || ((s1W || s1D) && emulate64))) {
            RegData dstLo, dstHi;
            splitToDW(dst, dstLo, dstHi);

            auto acc = g.acc0.retype(mulHiType)[dstLo.getOffset()](dstLo.getHS());

            g.mul(mod, acc, src0, lowWord(src1), loc);
            if (s1D)
                g.mach(mod, dstLo, src0, expandDW(src1), loc);
            else
                g.mach(mod, dstLo, src0, int32_t(0), loc);
            g.mov(mod, dstHi, dstLo, loc);
            g.mov(mod, dstLo, acc, loc);
        } else if (dstD && s0D && s1D && strategy.emulateDWxDW) {
            int ne1 = GRF::bytes(g.getHardware()) >> 2;

            for (int r = 0; r < mod.getExecSize(); r += ne1) {
                auto mmod = mod;
                mmod.setExecSize(std::min(mod.getExecSize() - r, ne1));

                auto acc = g.acc0.retype(mulHiType)[dst.getOffset()](dst.getHS());
                auto dummy = g.null.retype(mulHiType)[dst.getOffset()](dst.getHS());

                g.mul(mmod, acc, src0, lowWord(src1), loc);

                if (g.getHardware() < HW::Gen10) {
                    g.mach(mmod, dummy, src0, expandDW(src1), loc);
                    g.mov(mmod, dst, acc, loc);
                } else {
                    g.macl(mmod, dst, src0, expandDW(src1), loc);
                }

                regionVSAdvance(g.getHardware(), dst, ne1);
                regionVSAdvance(g.getHardware(), src0, ne1);
                regionVSAdvance(g.getHardware(), src1, ne1);
            }
        } else
            g.mul(mod, dst, src0, src1, loc);
    }

    template <typename DT = void, typename Generator>
    static void emul(Generator &g, const InstructionModifier &mod, const RegData &dst, const RegData &src0, const RegData &src1, const EmulationStrategy &strategy, const EmulationState &state, SourceLocation loc = {})
    {
        emulInternal<DT>(g, mod, dst, src0, src1, strategy, state, loc);
    }

    template <typename DT = void, typename Generator>
    static void emul(Generator &g, const InstructionModifier &mod, const RegData &dst, const RegData &src0, Immediate src1, const EmulationStrategy &strategy, const EmulationState &state, SourceLocation loc = {})
    {
        DataType t = src1.getType();
        if(t == DataType::ud || t == DataType::d || t == DataType::uw || t == DataType::w) {
            int64_t value = static_cast<int64_t>(uint64_t(src1.cast(DataType::q)));
            if (value == 0) {
                emov<DT>(g, mod, dst, uint16_t(0), strategy, loc);
                return;
            } else if (value == 1) {
                if (dst != src0) emov<DT>(g, mod, dst, src0, strategy, loc);
                return;
            } else if (utils::is_zero_or_pow2(value)) {
                eshl<DT>(g, mod, dst, src0, uint16_t(utils::log2(value)), strategy, state, loc);
                return;
            }
        }

        emulInternal<DT>(g, mod, dst, src0, src1, strategy, state, loc);
    }

    template <typename S1, typename Generator>
    static void emul32High(Generator &g, const InstructionModifier &mod, const RegData &dstHi, const RegData &src0, const S1 &src1, SourceLocation loc = {})
    {
        g.mul(mod, g.acc0.ud(dstHi.getOffset()), src0, lowWord(src1), loc);
        g.mach(mod, dstHi, src0, src1, loc);
    }

    // Shift left, emulating 64-bit arithmetic if configured.
    template <typename DT = void, typename Generator>
    static void eshl(Generator &g, const InstructionModifier &mod, RegData dst, RegData src0, uint16_t src1, const EmulationStrategy &strategy, const EmulationState &state, SourceLocation loc = {})
    {
        const auto &temp = state.temp;

        applyDefaultType<DT>(dst);
        applyDefaultType<DT>(src0);

        bool dstQ = isQW(dst);
        bool s0Q = isQW(src0);

        if (src1 == 0) {
            emov<DT, Generator>(g, mod, dst, src0, strategy, loc);
            return;
        }

        if (dstQ && strategy.emulate64 && !strategy.noemulate64_shift) {
            if (src1 >= 32) stub();

            RegData dstHi, dstLo, s0Hi, s0Lo;

            auto acc = temp[0].ud();

            splitToDW(dst, dstLo, dstHi);

            if (s0Q) {
                splitToDW(src0, s0Lo, s0Hi);

                g.shr(mod, acc, s0Lo, uint16_t(32 - src1), loc);
                g.shl(mod, dstHi, s0Hi, src1, loc);
                g.shl(mod, dstLo, s0Lo, src1, loc);
                g.or_(mod, dstHi, acc, dstHi, loc);
            } else {
                dstHi.setType(DataType::ud);
                g.shl(mod, dstLo, src0, src1, loc);
                g.shr(mod, dstHi, src0, uint16_t(32 - src1), loc);
            }
        } else {
            if (s0Q && !dstQ) downgradeToDW(src0);
            g.shl(mod, dst, src0, src1, loc);
        }
    }

    // Shift right, emulating 64-bit arithmetic if configured.
    template <typename DT = void, typename Generator>
    static void eshr(Generator &g, const InstructionModifier &mod, RegData dst, RegData src0, uint16_t src1, const EmulationStrategy &strategy, const EmulationState &state, SourceLocation loc = {})
    {
        const auto &temp = state.temp;

        applyDefaultType<DT>(dst);
        applyDefaultType<DT>(src0);

        bool dstQ = isQW(dst);
        bool s0Q = isQW(src0);

        if (src1 == 0) {
            emov<DT, Generator>(g, mod, dst, src0, strategy, loc);
            return;
        }

        if (dstQ && strategy.emulate64 && !strategy.noemulate64_shift) {
            if (src1 >= 32) stub();

            RegData dstHi, dstLo, s0Hi, s0Lo;

            auto acc = temp[0].ud();

            splitToDW(dst, dstLo, dstHi);

            if (s0Q) {
                splitToDW(src0, s0Lo, s0Hi);

                g.shl(mod, acc, s0Lo, uint16_t(32 - src1), loc);
                g.shr(mod, dstLo, s0Lo, src1, loc);
                isSigned(src0.getType()) ? g.asr(mod, dstHi, s0Hi, src1, loc)
                                         : g.shr(mod, dstHi, s0Hi, src1, loc);
                g.or_(mod, dstLo, acc, dstLo, loc);
            } else {
                dstLo.setType(dstHi.getType());
                isSigned(src0.getType()) ? g.asr(mod, dstLo, src0, src1, loc)
                                         : g.shr(mod, dstLo, src0, src1, loc);
                g.mov(mod, dstHi, uint16_t(0), loc);
            }
        } else {
            if (s0Q && !dstQ) downgradeToDW(src0);
            isSigned(src0.getType()) ? g.asr(mod, dst, src0, src1, loc)
                                     : g.shr(mod, dst, src0, src1, loc);
        }
    }

    // Multiply by a constant, optimizing for power-of-2 constants and emulating 64-bit arithmetic if configured.
    template <typename DT = void, typename Generator>
    static void emulConstant(Generator &g, const InstructionModifier &mod, const RegData &dst, const RegData &src0, int32_t src1, const EmulationStrategy &strategy, const EmulationState &state, SourceLocation loc = {})
    {
        if (src1 > 0)
            emul<DT>(g, mod, dst, src0, uint32_t(src1), strategy, state, loc);
        else
            emul<DT>(g, mod, dst, src0, int32_t(src1), strategy, state, loc);
    }
}; // struct EmulationHelper

} /* namespace NGEN_NAMESPACE */

#define NGEN_EMULATION_FORWARD \
template <typename DT = void> void emov(const NGEN_NAMESPACE::InstructionModifier &mod, NGEN_NAMESPACE::RegData dst, NGEN_NAMESPACE::RegData src0,   const EmulationStrategy &strategy)                                                                     { EmulationImplementation::emov<DT>(*this, mod, dst, src0, strategy); } \
template <typename DT = void> void emov(const NGEN_NAMESPACE::InstructionModifier &mod, NGEN_NAMESPACE::RegData dst, NGEN_NAMESPACE::Immediate src0, const EmulationStrategy &strategy)                                                                     { EmulationImplementation::emov<DT>(*this, mod, dst, src0, strategy); } \
template <typename DT = void> void eadd(const NGEN_NAMESPACE::InstructionModifier &mod, const NGEN_NAMESPACE::RegData &dst, const NGEN_NAMESPACE::RegData &src0, const NGEN_NAMESPACE::RegData &src1, const EmulationStrategy &strategy, const EmulationState &state) { EmulationImplementation::eadd<DT>(*this, mod, dst, src0, src1, strategy, state); } \
template <typename DT = void> void eadd(const NGEN_NAMESPACE::InstructionModifier &mod, const NGEN_NAMESPACE::RegData &dst, const NGEN_NAMESPACE::RegData &src0, NGEN_NAMESPACE::Immediate src1,      const EmulationStrategy &strategy, const EmulationState &state) { EmulationImplementation::eadd<DT>(*this, mod, dst, src0, src1, strategy, state); } \
template <typename DT = void> void emul(const NGEN_NAMESPACE::InstructionModifier &mod, const NGEN_NAMESPACE::RegData &dst, const NGEN_NAMESPACE::RegData &src0, const NGEN_NAMESPACE::RegData &src1, const EmulationStrategy &strategy, const EmulationState &state) { EmulationImplementation::emul<DT>(*this, mod, dst, src0, src1, strategy, state); } \
template <typename DT = void> void emul(const NGEN_NAMESPACE::InstructionModifier &mod, const NGEN_NAMESPACE::RegData &dst, const NGEN_NAMESPACE::RegData &src0, NGEN_NAMESPACE::Immediate src1,      const EmulationStrategy &strategy, const EmulationState &state) { EmulationImplementation::emul<DT>(*this, mod, dst, src0, src1, strategy, state); } \
template <typename DT = void> void eshl(const NGEN_NAMESPACE::InstructionModifier &mod, NGEN_NAMESPACE::RegData dst, NGEN_NAMESPACE::RegData src0, uint16_t src1, const EmulationStrategy &strategy, const EmulationState &state)                           { EmulationImplementation::eshl<DT>(*this, mod, dst, src0, src1, strategy, state); } \
template <typename DT = void> void eshr(const NGEN_NAMESPACE::InstructionModifier &mod, NGEN_NAMESPACE::RegData dst, NGEN_NAMESPACE::RegData src0, uint16_t src1, const EmulationStrategy &strategy, const EmulationState &state)                           { EmulationImplementation::eshr<DT>(*this, mod, dst, src0, src1, strategy, state); } \
template <typename DT = void> void emulConstant(const NGEN_NAMESPACE::InstructionModifier &mod, const NGEN_NAMESPACE::RegData &dst, const NGEN_NAMESPACE::RegData &src0, int32_t src1, const EmulationStrategy &strategy, const EmulationState &state)      { EmulationImplementation::emulConstant<DT>(*this, mod, dst, src0, src1, strategy, state); } \
template <typename S1> void emul32High(const NGEN_NAMESPACE::InstructionModifier &mod, const NGEN_NAMESPACE::RegData &dstHi, const NGEN_NAMESPACE::RegData &src0, const S1 &src1)                                                                           { EmulationImplementation::emul32High(*this, mod, dstHi, src0, src1); }
#endif
