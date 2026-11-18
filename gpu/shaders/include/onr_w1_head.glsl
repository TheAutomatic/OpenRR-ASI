// SPDX-License-Identifier: MIT
// One attention head of the W1 body (onr_block_w1.glsl), hd in scope: once in its loop, or once per head with hd a
// constant (LT_FAR1), so each head keeps only its own key tiles.
        uint wq = hd * 32u * ENC_C;
#ifdef LT_W1A8
        // WP-D5 (r2): one head fully on exact-e4m3 weights, f32 accumulators, 8-cycle FP8 WMMA
        // only. ENC_C = 32 is TWO K tiles: n = 0 (channels 0..15, xq<t>0 @ wq) and n = 1
        // (channels 16..31, xq<t>1 @ wq + 16) - every product below covers both.
        QA8 wqa00, wqa01, wqa10, wqa11;   // Wq A tiles [dim half dh][K tile n]
        coopMatLoad(wqa00, W1_QA8, wq, ENC_C, ROW);
        coopMatLoad(wqa01, W1_QA8, wq + 16u * ENC_C, ENC_C, ROW);
        coopMatLoad(wqa10, W1_QA8, wq + 16u, ENC_C, ROW);
        coopMatLoad(wqa11, W1_QA8, wq + 16u * ENC_C + 16u, ENC_C, ROW);
        // Q^T per token tile, narrow scope: only the two accumulators of one tile live (16 VGPR)
        QB8 qk00, qk01, qk10, qk11, qk20, qk21, qk30, qk31;   // qk<token tile><dim half>
        {
            QC8 t0_ = QC8(0.0), t1_ = QC8(0.0);
            MMA(t0_, wqa00, xq00) MMA(t1_, wqa01, xq00) MMA(t0_, wqa10, xq01) MMA(t1_, wqa11, xq01)
            W1A_Q8(qk00, t0_) W1A_Q8(qk01, t1_)
        }
        {
            QC8 t0_ = QC8(0.0), t1_ = QC8(0.0);
            MMA(t0_, wqa00, xq10) MMA(t1_, wqa01, xq10) MMA(t0_, wqa10, xq11) MMA(t1_, wqa11, xq11)
            W1A_Q8(qk10, t0_) W1A_Q8(qk11, t1_)
        }
        {
            QC8 t0_ = QC8(0.0), t1_ = QC8(0.0);
            MMA(t0_, wqa00, xq20) MMA(t1_, wqa01, xq20) MMA(t0_, wqa10, xq21) MMA(t1_, wqa11, xq21)
            W1A_Q8(qk20, t0_) W1A_Q8(qk21, t1_)
        }
        {
            QC8 t0_ = QC8(0.0), t1_ = QC8(0.0);
            MMA(t0_, wqa00, xq30) MMA(t1_, wqa01, xq30) MMA(t0_, wqa10, xq31) MMA(t1_, wqa11, xq31)
            W1A_Q8(qk30, t0_) W1A_Q8(qk31, t1_)
        }
        // V[tokens][dim half] = sum_n x Wv^T; quantized straight into A fragments (V^T, free transpose)
        QB8 wvb00, wvb01, wvb10, wvb11;   // Wv B tiles [K tile n][dim half dh]
        coopMatLoad(wvb00, W1_VB8, wq, ENC_C, COL);
        coopMatLoad(wvb01, W1_VB8, wq + 16u * ENC_C, ENC_C, COL);
        coopMatLoad(wvb10, W1_VB8, wq + 16u, ENC_C, COL);
        coopMatLoad(wvb11, W1_VB8, wq + 16u * ENC_C + 16u, ENC_C, COL);
        QA8 vt00, vt01, vt10, vt11, vt20, vt21, vt30, vt31;   // V^T A tiles [dim half][token tile]
        {
            QA8 a0_, a1_;
            W1A_TA(a0_, xq00) W1A_TA(a1_, xq01)
            QC8 u0_ = QC8(0.0), u1_ = QC8(0.0);
            MMA(u0_, a0_, wvb00) MMA(u1_, a0_, wvb01) MMA(u0_, a1_, wvb10) MMA(u1_, a1_, wvb11)
            W1A_Q8A(vt00, u0_) W1A_Q8A(vt01, u1_)
        }
        {
            QA8 a0_, a1_;
            W1A_TA(a0_, xq10) W1A_TA(a1_, xq11)
            QC8 u0_ = QC8(0.0), u1_ = QC8(0.0);
            MMA(u0_, a0_, wvb00) MMA(u1_, a0_, wvb01) MMA(u0_, a1_, wvb10) MMA(u1_, a1_, wvb11)
            W1A_Q8A(vt10, u0_) W1A_Q8A(vt11, u1_)
        }
        {
            QA8 a0_, a1_;
            W1A_TA(a0_, xq20) W1A_TA(a1_, xq21)
            QC8 u0_ = QC8(0.0), u1_ = QC8(0.0);
            MMA(u0_, a0_, wvb00) MMA(u1_, a0_, wvb01) MMA(u0_, a1_, wvb10) MMA(u1_, a1_, wvb11)
            W1A_Q8A(vt20, u0_) W1A_Q8A(vt21, u1_)
        }
        {
            QA8 a0_, a1_;
            W1A_TA(a0_, xq30) W1A_TA(a1_, xq31)
            QC8 u0_ = QC8(0.0), u1_ = QC8(0.0);
            MMA(u0_, a0_, wvb00) MMA(u1_, a0_, wvb01) MMA(u0_, a1_, wvb10) MMA(u1_, a1_, wvb11)
            W1A_Q8A(vt30, u0_) W1A_Q8A(vt31, u1_)
        }
        // S = Q_keys Q_own^T + bias per own tile: A = the key tile's transpose, B = the own tile
#define W1A_S(s_, ot, j) { MH bh_; coopMatLoad(bh_, pc.bias.v, hd * 4096u + (ot) * 1024u + (j) * 16u, 64u, COL); s_ = QC8(ACC(bh_)); \
            IFK(j, ot) { QA8 a_; W1A_TA(a_, qk##j##0) MMA(s_, a_, qk##ot##0) W1A_TA(a_, qk##j##1) MMA(s_, a_, qk##ot##1) } }
        // the softmax's probabilities as f32 products straight into the pair quantizer
#define W1A_PB(pbj, pj, rf) { [[unroll]] for (int e = 0; e < 8; e += 2) { \
            vec2 w_ = vec2(float(pj[e]), float(pj[e + 1])) * (rf); \
            fe4m3vec2 q_ = qe4_pair32(w_); pbj[e] = q_.x, pbj[e + 1] = q_.y; } }
        // O^T[dim half][own] = sum over KEPT key tiles V^T x P (sparse guard restored)
#define W1A_OKT(ot, kt) IFK(uint(ot), uint(kt)) { MMA(oh0_, vt##kt##0, pb##kt) MMA(oh1_, vt##kt##1, pb##kt) }
        // W_out from the UT8 tail, this head's columns (hd * 32 offsets)
#define W1A_OW(ot) { QB8 oh80_, oh81_; W1A_Q8(oh80_, oh0_) W1A_Q8(oh81_, oh1_) QA8 w_; \
            coopMatLoad(w_, W1_WOUT8, hd * 32u, ENC_X, ROW); MMA(oc##ot##0, w_, oh80_) \
            coopMatLoad(w_, W1_WOUT8, hd * 32u + 16u, ENC_X, ROW); MMA(oc##ot##0, w_, oh81_) \
            coopMatLoad(w_, W1_WOUT8, 16u * ENC_X + hd * 32u, ENC_X, ROW); MMA(oc##ot##1, w_, oh80_) \
            coopMatLoad(w_, W1_WOUT8, 16u * ENC_X + hd * 32u + 16u, ENC_X, ROW); MMA(oc##ot##1, w_, oh81_) }
#define W1A_TAIL(ot) { float rf_ = float(RCP_SUM(sum)); \
            QB8 pb0, pb1, pb2, pb3; \
            W1A_PB(pb0, p0, rf_) W1A_PB(pb1, p1, rf_) W1A_PB(pb2, p2, rf_) W1A_PB(pb3, p3, rf_) \
            QC8 oh0_ = QC8(0.0), oh1_ = QC8(0.0); \
            W1A_OKT(ot, 0) W1A_OKT(ot, 1) W1A_OKT(ot, 2) W1A_OKT(ot, 3) \
            W1A_OW(ot) }
#define W1A_ATT(ot) if (lv##ot) { \
            QC8 s0_, s1_, s2_, s3_; \
            W1A_S(s0_, ot, 0) W1A_S(s1_, ot, 1) W1A_S(s2_, ot, 2) W1A_S(s3_, ot, 3) \
            MH p0 = MH(s0_), p1 = MH(s1_), p2 = MH(s2_), p3 = MH(s3_); \
            f16vec2 e2[4]; \
            [[unroll]] for (int e = 0; e < 8; e += 2) { \
                f16vec2 t0 = TK(ot, 0u, unpackFloat2x16(exp_bits2(f16vec2(p0[e], p0[e + 1])))); \
                f16vec2 t1 = TK(ot, 1u, unpackFloat2x16(exp_bits2(f16vec2(p1[e], p1[e + 1])))); \
                f16vec2 t2 = TK(ot, 2u, unpackFloat2x16(exp_bits2(f16vec2(p2[e], p2[e + 1])))); \
                f16vec2 t3 = TK(ot, 3u, unpackFloat2x16(exp_bits2(f16vec2(p3[e], p3[e + 1])))); \
                p0[e] = t0.x, p0[e + 1] = t0.y, p1[e] = t1.x, p1[e + 1] = t1.y; \
                p2[e] = t2.x, p2[e + 1] = t2.y, p3[e] = t3.x, p3[e + 1] = t3.y; \
                precise f16vec2 sm = (t0 + t1) + (t2 + t3); \
                e2[e >> 1] = sm; } \
            SM_SUM(sum, e2, p0, p1, p2, p3) \
            W1A_TAIL(ot) }
#ifdef LT_SYM
        // symmetric scores: each pair once through the FP8 products. A = the KEY tile's
        // transpose, B = the OWN tile (bias at T * 1024 + K * 16 holds [keys K][own T])
#define W1A_EDIR(pT, T, K) { MH bh_; coopMatLoad(bh_, pc.bias.v, hd * 4096u + (T) * 1024u + (K) * 16u, 64u, COL); QC8 p_ = QC8(ACC(bh_)); \
            QA8 a_; W1A_TA(a_, qk##K##0) MMA(p_, a_, qk##T##0) W1A_TA(a_, qk##K##1) MMA(p_, a_, qk##T##1) \
            pT = MH(p_); \
            [[unroll]] for (int e = 0; e < 8; e += 2) { f16vec2 t_ = unpackFloat2x16(exp_bits2(f16vec2(pT[e], pT[e + 1]))); pT[e] = t_.x, pT[e + 1] = t_.y; } }
#define W1A_EUP(T, K) MH e##T##K = MH(0.0); if (KEPT(uint(T), uint(K)) && (lv##T || lv##K)) W1A_EDIR(e##T##K, T, K)
#define W1A_EOWN(pT, T) MH pT = MH(0.0); W1A_EDIR(pT, T, T)
        // the lower tiles reuse the f16 exp registers through the EXACT fp16 identity (never
        // quantize the unnormalized exp into e4m3: it spans [6e-8, 65504], far past +-448/2^-9)
#define W1A_ELO(pT, T, K) MH pT = MH(0.0); if (KEPT(uint(T), uint(K))) { MA a_; CPA(a_, e##K##T) MMA(pT, a_, idb) }
#define W1A_ATTN(ot) if (lv##ot) { \
            SM_E2(e2, p0, p1, p2, p3) SM_SUMR(sum, e2, p0, p1, p2, p3) \
            W1A_TAIL(ot) }
        W1A_EUP(0, 1) W1A_EUP(0, 2) W1A_EUP(0, 3) W1A_EUP(1, 2) W1A_EUP(1, 3) W1A_EUP(2, 3)
        if (lv0) { W1A_EOWN(p0, 0) MH p1 = e01, p2 = e02, p3 = e03; W1A_ATTN(0) }
        if (lv1) { W1A_ELO(p0, 1, 0) W1A_EOWN(p1, 1) MH p2 = e12, p3 = e13; W1A_ATTN(1) }
        if (lv2) { W1A_ELO(p0, 2, 0) W1A_ELO(p1, 2, 1) W1A_EOWN(p2, 2) MH p3 = e23; W1A_ATTN(2) }
        if (lv3) { W1A_ELO(p0, 3, 0) W1A_ELO(p1, 3, 1) W1A_ELO(p2, 3, 2) W1A_EOWN(p3, 3) W1A_ATTN(3) }
#else
        W1A_ATT(0) W1A_ATT(1) W1A_ATT(2) W1A_ATT(3)
#endif
#undef W1A_S
#undef W1A_PB
#undef W1A_OKT
#undef W1A_OW
#undef W1A_TAIL
#undef W1A_ATT
#ifdef LT_SYM
#undef W1A_EDIR
#undef W1A_EUP
#undef W1A_EOWN
#undef W1A_ELO
#undef W1A_ATTN
#endif
#else
        // Q^T [dim tile][token tile] = in_a x^T, V [token tile][dim tile] = x in_b^T; K = channel tiles in order
#if defined(LT_VO8) && defined(LT_VO)
        // LT_VO8: compute V before Q using e4m3(vo_values) at 5n bytes in a non-unrolled token loop
        // so only one QC8 pair (s0_, s1_) and one QA8 pair (vx0_, vx1_) exist before Q allocates.
        MH v00 = MH(0.0), v01 = MH(0.0), v10 = MH(0.0), v11 = MH(0.0), v20 = MH(0.0), v21 = MH(0.0), v30 = MH(0.0), v31 = MH(0.0);
        {
#define W1_VB8 E4View(uint64_t(pc.in_b) + uint64_t(VB8_LR(0u))).v
            QB8 wb00, wb01, wb10, wb11;
            coopMatLoad(wb00, W1_VB8, wq, ENC_C, COL);
            coopMatLoad(wb01, W1_VB8, wq + 16u * ENC_C, ENC_C, COL);
            coopMatLoad(wb10, W1_VB8, wq + 16u, ENC_C, COL);
            coopMatLoad(wb11, W1_VB8, wq + 16u * ENC_C + 16u, ENC_C, COL);
            [[dont_unroll]] for (uint vt_ = 0u; vt_ < 4u; ++vt_) {
                QA8 vx0_, vx1_;
                [[unroll]] for (int i_ = 0; i_ < 8; i_ += 2) {
                    f16vec2 ha_ = vt_ < 2u ? (vt_ == 0u ? f16vec2(x00[i_], x00[i_ + 1]) : f16vec2(x10[i_], x10[i_ + 1]))
                                           : (vt_ == 2u ? f16vec2(x20[i_], x20[i_ + 1]) : f16vec2(x30[i_], x30[i_ + 1]));
                    f16vec2 hb_ = vt_ < 2u ? (vt_ == 0u ? f16vec2(x01[i_], x01[i_ + 1]) : f16vec2(x11[i_], x11[i_ + 1]))
                                           : (vt_ == 2u ? f16vec2(x21[i_], x21[i_ + 1]) : f16vec2(x31[i_], x31[i_ + 1]));
                    fe4m3vec2 q0_ = qe4_pair32(vec2(ha_)), q1_ = qe4_pair32(vec2(hb_));
                    vx0_[i_] = q0_.x, vx0_[i_ + 1] = q0_.y, vx1_[i_] = q1_.x, vx1_[i_ + 1] = q1_.y;
                }
                QC8 s0_ = QC8(0.0), s1_ = QC8(0.0);
                MMA(s0_, vx0_, wb00) MMA(s1_, vx0_, wb01) MMA(s0_, vx1_, wb10) MMA(s1_, vx1_, wb11)
                MH va_ = MH(s0_), vb_ = MH(s1_);
                if (vt_ == 0u) { [[unroll]] for (int i_ = 0; i_ < 8; ++i_) v00[i_] = va_[i_], v01[i_] = vb_[i_]; }
                else if (vt_ == 1u) { [[unroll]] for (int i_ = 0; i_ < 8; ++i_) v10[i_] = va_[i_], v11[i_] = vb_[i_]; }
                else if (vt_ == 2u) { [[unroll]] for (int i_ = 0; i_ < 8; ++i_) v20[i_] = va_[i_], v21[i_] = vb_[i_]; }
                else { [[unroll]] for (int i_ = 0; i_ < 8; ++i_) v30[i_] = va_[i_], v31[i_] = vb_[i_]; }
            }
#undef W1_VB8
        }
        ACC q00 = ACC(0.0), q01 = ACC(0.0), q10 = ACC(0.0), q11 = ACC(0.0), q20 = ACC(0.0), q21 = ACC(0.0), q30 = ACC(0.0), q31 = ACC(0.0);
#define QK_TOK(xt, qa, qb) { MB b_; XPB(b_, xt) MMA(qa, wa0, b_) Q2_IF { MMA(qb, wa1, b_) } }
#define QK(n, xa, xb, xc, xd) { MA wa0, wa1; \
            coopMatLoad(wa0, pc.in_a.v, QA_LR(wq + (n) * 16u), ENC_C, ROW); Q2_IF { coopMatLoad(wa1, pc.in_a.v, wq + 16u * ENC_C + (n) * 16u, ENC_C, ROW); } \
            QK_TOK(xa, q00, q01) QK_TOK(xb, q10, q11) QK_TOK(xc, q20, q21) QK_TOK(xd, q30, q31) }
        QK(0u, x00, x10, x20, x30)
        QK(1u, x01, x11, x21, x31)
#undef QK_TOK
#undef QK
#else
        ACC q00 = ACC(0.0), q01 = ACC(0.0), q10 = ACC(0.0), q11 = ACC(0.0), q20 = ACC(0.0), q21 = ACC(0.0), q30 = ACC(0.0), q31 = ACC(0.0);
        ACC v00 = ACC(0.0), v01 = ACC(0.0), v10 = ACC(0.0), v11 = ACC(0.0), v20 = ACC(0.0), v21 = ACC(0.0), v30 = ACC(0.0), v31 = ACC(0.0);
#define QV_TOK(xt, qa, qb, va, vb) { MB b_; XPB(b_, xt) MMA(qa, wa0, b_) Q2_IF { MMA(qb, wa1, b_) } \
            MA a_; XPA(a_, xt) MMA(va, a_, wb0) MMA(vb, a_, wb1) }
#define QV_K(n, xa, xb, xc, xd) { MA wa0, wa1; MB wb0, wb1; \
            coopMatLoad(wa0, pc.in_a.v, QA_LR(wq + (n) * 16u), ENC_C, ROW); Q2_IF { coopMatLoad(wa1, pc.in_a.v, wq + 16u * ENC_C + (n) * 16u, ENC_C, ROW); } \
            coopMatLoad(wb0, pc.in_b.v, VB_LR(wq + (n) * 16u), ENC_C, COL); coopMatLoad(wb1, pc.in_b.v, VB_LR(wq + 16u * ENC_C + (n) * 16u), ENC_C, COL); \
            QV_TOK(xa, q00, q01, v00, v01) QV_TOK(xb, q10, q11, v10, v11) QV_TOK(xc, q20, q21, v20, v21) QV_TOK(xd, q30, q31, v30, v31) }
        QV_K(0u, x00, x10, x20, x30)
        QV_K(1u, x01, x11, x21, x31)
#endif
#ifndef LT_VO
        // out projection fragments of the head: [channel tile][dim tile], K = 32 hd + dims
        MA wo00, wo01, wo10, wo11;
        coopMatLoad(wo00, pc.wout.v, WT_OUT(hd * 32u), WS_OUT, ROW);
        coopMatLoad(wo01, pc.wout.v, WT_OUT(hd * 32u + 16u), WS_OUT, ROW);
        coopMatLoad(wo10, pc.wout.v, WT_OUT(16u * ENC_X + hd * 32u), WS_OUT, ROW);
        coopMatLoad(wo11, pc.wout.v, WT_OUT(16u * ENC_X + hd * 32u + 16u), WS_OUT, ROW);
#endif
        // own token tile: S^T [64 keys][16] = Q_keys Q_own^T + bias^T, softmax in registers (the transposed body's
        // tree), O^T = V^T P^T, then the out projection into its accumulators
#define ATT(r0_, qo0, qo1, ca0, ca1) { \
            MH bh; \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u, 64u, COL); ACC s0 = ACC(bh); \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u + 16u, 64u, COL); ACC s1 = ACC(bh); \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u + 32u, 64u, COL); ACC s2 = ACC(bh); \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u + 48u, 64u, COL); ACC s3 = ACC(bh); \
            { MB b_; MA a_; CPB(b_, qo0) \
              IFK((r0_) >> 4, 0u) { CPA(a_, q00) MMA(s0, a_, b_) } IFK((r0_) >> 4, 1u) { CPA(a_, q10) MMA(s1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { CPA(a_, q20) MMA(s2, a_, b_) } IFK((r0_) >> 4, 3u) { CPA(a_, q30) MMA(s3, a_, b_) } \
              Q2_IF { CPB(b_, qo1) \
              IFK((r0_) >> 4, 0u) { CPA(a_, q01) MMA(s0, a_, b_) } IFK((r0_) >> 4, 1u) { CPA(a_, q11) MMA(s1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { CPA(a_, q21) MMA(s2, a_, b_) } IFK((r0_) >> 4, 3u) { CPA(a_, q31) MMA(s3, a_, b_) } } } \
            MH p0 = MH(s0), p1 = MH(s1), p2 = MH(s2), p3 = MH(s3); \
            f16vec2 e2[4]; \
            [[unroll]] for (int e = 0; e < 8; e += 2) { \
                f16vec2 t0 = TK((r0_) >> 4, 0u, unpackFloat2x16(exp_bits2(f16vec2(p0[e], p0[e + 1])))); \
                f16vec2 t1 = TK((r0_) >> 4, 1u, unpackFloat2x16(exp_bits2(f16vec2(p1[e], p1[e + 1])))); \
                f16vec2 t2 = TK((r0_) >> 4, 2u, unpackFloat2x16(exp_bits2(f16vec2(p2[e], p2[e + 1])))); \
                f16vec2 t3 = TK((r0_) >> 4, 3u, unpackFloat2x16(exp_bits2(f16vec2(p3[e], p3[e + 1])))); \
                p0[e] = t0.x, p0[e + 1] = t0.y, p1[e] = t1.x, p1[e + 1] = t1.y; \
                p2[e] = t2.x, p2[e + 1] = t2.y, p3[e] = t3.x, p3[e + 1] = t3.y; \
                precise f16vec2 sm = (t0 + t1) + (t2 + t3); \
                e2[e >> 1] = sm; \
            } \
            SM_SUM(sum, e2, p0, p1, p2, p3) \
            float16_t r = RCP_SUM(sum); \
            [[unroll]] for (int e = 0; e < 8; e += 2) { \
                precise f16vec2 u0 = f16vec2(p0[e], p0[e + 1]) * r, u1 = f16vec2(p1[e], p1[e + 1]) * r; \
                precise f16vec2 u2 = f16vec2(p2[e], p2[e + 1]) * r, u3 = f16vec2(p3[e], p3[e + 1]) * r; \
                p0[e] = u0.x, p0[e + 1] = u0.y, p1[e] = u1.x, p1[e + 1] = u1.y; \
                p2[e] = u2.x, p2[e + 1] = u2.y, p3[e] = u3.x, p3[e + 1] = u3.y; \
            } \
            ATT_OUT(r0_, ca0, ca1) }
#ifdef LT_VO
// the value path is linear: V_h already through wout_h in_b_h (Engine::upload_stage), so P V_h goes straight into the out
// accumulators, without the 16 products per head of the out projection
#define ATT_OUT(r0_, o0, o1) { MB b_; MA a_; \
              IFK((r0_) >> 4, 0u) { CPB(b_, p0) CPA(a_, v00) MMA(o0, a_, b_) CPA(a_, v01) MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 1u) { CPB(b_, p1) CPA(a_, v10) MMA(o0, a_, b_) CPA(a_, v11) MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { CPB(b_, p2) CPA(a_, v20) MMA(o0, a_, b_) CPA(a_, v21) MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 3u) { CPB(b_, p3) CPA(a_, v30) MMA(o0, a_, b_) CPA(a_, v31) MMA(o1, a_, b_) } }
#else
#define ATT_OUT(r0_, ca0, ca1) ACC o0 = ACC(0.0), o1 = ACC(0.0); \
            { MB b_; MA a_; \
              IFK((r0_) >> 4, 0u) { CPB(b_, p0) CPA(a_, v00) MMA(o0, a_, b_) CPA(a_, v01) MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 1u) { CPB(b_, p1) CPA(a_, v10) MMA(o0, a_, b_) CPA(a_, v11) MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { CPB(b_, p2) CPA(a_, v20) MMA(o0, a_, b_) CPA(a_, v21) MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 3u) { CPB(b_, p3) CPA(a_, v30) MMA(o0, a_, b_) CPA(a_, v31) MMA(o1, a_, b_) } \
              MH oh = MH(o0); CPB(b_, oh) MMA(ca0, wo00, b_) MMA(ca1, wo10, b_) \
              oh = MH(o1); CPB(b_, oh) MMA(ca0, wo01, b_) MMA(ca1, wo11, b_) }
#endif
#ifdef LT_SYM
        // LT_SYM: the weights before the normalisation are symmetric (S = Q Q^T + a symmetric bias), so a pair of token tiles
        // goes through the bias, S and the exponential once. The upper tile's [keys][own] is the lower one's transposed, and an
        // accumulator copied is the A operand of its transpose: one product with the identity gives it back as an accumulator
#define E_DIR(p, T, K) { coopMatLoad(p, pc.bias.v, hd * 4096u + uint(T) * 1024u + uint(K) * 16u, 64u, COL); \
            { MB b_; MA a_; CPB(b_, q##T##0) CPA(a_, q##K##0) MMA(p, a_, b_) Q2_IF { CPB(b_, q##T##1) CPA(a_, q##K##1) MMA(p, a_, b_) } } \
            [[unroll]] for (int e = 0; e < 8; e += 2) { f16vec2 t_ = unpackFloat2x16(exp_bits2(f16vec2(p[e], p[e + 1]))); p[e] = t_.x, p[e + 1] = t_.y; } }
#define E_UP(T, K) MH e##T##K = MH(0.0); if (KEPT(uint(T), uint(K)) && (lv##T || lv##K)) E_DIR(e##T##K, T, K)
#define E_OWN(p, T) MH p = MH(0.0); E_DIR(p, T, T)
#define E_LO(p, T, K) MH p = MH(0.0); if (KEPT(uint(T), uint(K))) { MA a_; CPA(a_, e##K##T) MMA(p, a_, idb) }
        E_UP(0, 1) E_UP(0, 2) E_UP(0, 3) E_UP(1, 2) E_UP(1, 3) E_UP(2, 3)
#define ATT_N(r0_, ca0, ca1) { \
            SM_E2(e2, p0, p1, p2, p3) \
            SM_SUMR(sum, e2, p0, p1, p2, p3) \
            float16_t r = RCP_SUM(sum); \
            [[unroll]] for (int e = 0; e < 8; e += 2) { \
                precise f16vec2 u0 = f16vec2(p0[e], p0[e + 1]) * r, u1 = f16vec2(p1[e], p1[e + 1]) * r; \
                precise f16vec2 u2 = f16vec2(p2[e], p2[e + 1]) * r, u3 = f16vec2(p3[e], p3[e + 1]) * r; \
                p0[e] = u0.x, p0[e + 1] = u0.y, p1[e] = u1.x, p1[e + 1] = u1.y; \
                p2[e] = u2.x, p2[e + 1] = u2.y, p3[e] = u3.x, p3[e + 1] = u3.y; \
            } \
            ATT_OUT(r0_, ca0, ca1) }
        if (lv0) { E_OWN(p0, 0) MH p1 = e01, p2 = e02, p3 = e03; ATT_N(0u, c00, c01) }
        if (lv1) { E_LO(p0, 1, 0) E_OWN(p1, 1) MH p2 = e12, p3 = e13; ATT_N(16u, c10, c11) }
        if (lv2) { E_LO(p0, 2, 0) E_LO(p1, 2, 1) E_OWN(p2, 2) MH p3 = e23; ATT_N(32u, c20, c21) }
        if (lv3) { E_LO(p0, 3, 0) E_LO(p1, 3, 1) E_LO(p2, 3, 2) E_OWN(p3, 3) ATT_N(48u, c30, c31) }
#else
        if (lv0) ATT(0u, q00, q01, c00, c01)
        if (lv1) ATT(16u, q10, q11, c10, c11)
        if (lv2) ATT(32u, q20, q21, c20, c21)
        if (lv3) ATT(48u, q30, q31, c30, c31)
#endif
#endif  // LT_W1A8
