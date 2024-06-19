#include "layers.h"
#include <algorithm>
#include <cmath>
#include <codecvt>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <random>
#include <regex>
#include <string>
#include <functional>

#ifdef GGML_USE_CLBLAST
#include "ggml-opencl.h"
#endif

#define ggctx       (ctx->gctx.get())

#undef MIN
#undef MAX

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

namespace chatllm
{

#include "custom_ops.cpp"

    ggml_tensor *inplace_act(ggml_context *ctx, ActFunc act, ggml_tensor *input)
    {
        switch (act)
        {
        case ActFunc::GELU:
            return ggml_gelu_inplace(ctx, input);
        case ActFunc::SILU:
            return ggml_silu_inplace(ctx, input);
        case ActFunc::Tanh:
            return ggml_tanh_inplace(ctx, input);
        case ActFunc::RELU:
            return ggml_relu_inplace(ctx, input);
        case ActFunc::RELU2:
            {
                ggml_tensor *output = ggml_relu_inplace(ctx, input);
                output = ggml_sqr_inplace(ctx, output);
                return output;
            }
        default:
            CHATLLM_CHECK(false) << "not implemented act function: " << act;
            return NULL;
        }
    }

    ggml_tensor *Embedding::forward(ForwardContext *ctx, ggml_tensor *input)
    {
        ggml_tensor *output = (ggml_n_dims(input) == 1) && (ggml_type::GGML_TYPE_I32 == input->type)
                                ? ggml_get_rows(ctx->gctx.get(), weight, input)
                                : ggml_mul_mat(ctx->gctx.get(), weight, input);
        return output;
    }

    ggml_tensor *RobertaEmbedding::forward(ForwardContext *ctx, ggml_tensor *input, int n_past)
    {
        int qlen = (int)input->ne[0];
        ggml_tensor *idx = ggml_view_1d(ggctx, indices, qlen, (n_past + pad_index) * ggml_element_size(indices));

        ggml_tensor *output1 = ggml_get_rows(ggctx, word_weight, input);
        ggml_tensor *output2 = ggml_get_rows(ggctx, position_weight, idx);

        ggml_tensor *output = ggml_add_inplace(ggctx, output1, output2);

        output = ln.forward(ctx, output);
        return output;
    }

    ggml_tensor *Linear::forward(ForwardContext *ctx, ggml_tensor *input)
    {
        // input: [seqlen, in_features]
        ggml_tensor *output = ggml_mul_mat(ctx->gctx.get(), weight, input); // [seqlen, out_features]
        ggml_mul_mat_set_prec(output, prec);
        if (bias)
        {
            output = ggml_add_inplace(ctx->gctx.get(), output, bias);
        }
        return output;
    }

    ggml_tensor *LayerNorm::forward(ForwardContext *ctx, ggml_tensor *input)
    {
        // input: [seqlen, normalized_shape]
        ggml_tensor *output = ggml_norm_inplace(ctx->gctx.get(), input, eps);
        output = ggml_mul_inplace(ctx->gctx.get(), output, weight);
        if (bias)
            output = ggml_add_inplace(ctx->gctx.get(), output, bias);
        return output;
    }

    ggml_tensor *RMSNorm::forward(ForwardContext *ctx, ggml_tensor *input)
    {
        ggml_tensor *output = ggml_rms_norm_inplace(ctx->gctx.get(), input, eps);
        output = ggml_mul_inplace(ctx->gctx.get(), output, weight);
        return output;
    }

    ggml_tensor *RobertaPooler::forward(ForwardContext *ctx, ggml_tensor *hidden_states)
    {
        int hidden_size = (int)hidden_states->ne[0];

        // We "pool" the model by simply taking the hidden state corresponding to the first token.
        ggml_tensor *first_token_tensor = ggml_view_2d(ggctx, hidden_states, hidden_size, 1,
                                                      hidden_size * ggml_element_size(hidden_states), 0);
        ggml_tensor *output = dense.forward(ctx, first_token_tensor);
        output = inplace_act(ggctx, act, output);
        return output;
    }

    ggml_tensor *RobertaClassificationHead::forward(ForwardContext *ctx, ggml_tensor *hidden_states)
    {
        int hidden_size = (int)hidden_states->ne[0];

        // We "pool" the model by simply taking the hidden state corresponding to the first token.
        ggml_tensor *first_token_tensor = ggml_view_2d(ggctx, hidden_states, hidden_size, 1,
                                                      hidden_size * ggml_element_size(hidden_states), 0);
        ggml_tensor *output = dense.forward(ctx, first_token_tensor);
        output = inplace_act(ggctx, act, output);
        output = out_proj.forward(ctx, output);
        output = ggml_map_custom1(ggctx, output, ggml_compute_forward_sigmoid, 1, nullptr);
        return output;
    }

    ggml_tensor *BCEFinalNorm::forward(ForwardContext *ctx, ggml_tensor *hidden_states)
    {
        int hidden_size = (int)hidden_states->ne[0];
        ggml_tensor *first_token_tensor = ggml_view_1d(ggctx, hidden_states, hidden_size, 0);
        ggml_tensor *output = ggml_map_custom1(ggctx, first_token_tensor, ggml_compute_forward_simple_norm, 1, this);
        return output;
    }

    void fill_pos_vector(ggml_tensor *pos, int n_past, int qlen)
    {
        int *p = (int *)pos->data;
        for (int i = 0; i < qlen; i++)
            p[i] = n_past + i;
        pos->ne[0] = qlen;
    }

    ggml_tensor *GLMSelfAttention::forward(ForwardContext *ctx, ggml_tensor *hidden_states, int n_past)
    {
        int hidden_size = (int)hidden_states->ne[0];
        int qlen = (int)hidden_states->ne[1];
        int head_size = hidden_size / num_attention_heads;
        int rope_dim = head_size / 2;
        fill_pos_vector(pos, n_past, qlen);

        if (shift_pending.shift > 0)
        {
            int remain = shift_pending.total - shift_pending.shift;
            if (remain > 0)
            {
                struct ggml_tensor * k_cache_remain = ggml_view_3d(ctx->gctx.get(), k_cache, head_size, remain, num_attention_heads, k_cache->nb[1], k_cache->nb[2],
                         shift_pending.shift * head_size * ggml_element_size(k_cache)); // [heads, remain, head_size]
                struct ggml_tensor * k_cache_dst    = ggml_view_3d(ctx->gctx.get(), k_cache, head_size, remain, num_attention_heads, k_cache->nb[1], k_cache->nb[2],
                         0); // [heads, remain, head_size]

                struct ggml_tensor * v_cache_remain = ggml_view_3d(ctx->gctx.get(), v_cache, remain, head_size, num_attention_heads, v_cache->nb[1], v_cache->nb[2],
                         shift_pending.shift * ggml_element_size(v_cache)); // [heads, head_size, remain]
                struct ggml_tensor * v_cache_dst    = ggml_view_3d(ctx->gctx.get(), v_cache, remain, head_size, num_attention_heads, v_cache->nb[1], v_cache->nb[2],
                         0); // [heads, head_size, remain]

                ggml_build_forward_expand(ctx->gf, ggml_cpy(ctx->gctx.get(), k_cache_remain, k_cache_dst));
                ggml_build_forward_expand(ctx->gf, ggml_cpy(ctx->gctx.get(), v_cache_remain, v_cache_dst));
            }

            shift_pending.clear();
        }

        ggml_tensor *qkv = query_key_value.forward(ctx, hidden_states); // [qlen, 3 * hidden]

        ggml_tensor *query_layer = ggml_view_3d(ctx->gctx.get(), qkv, head_size, num_attention_heads, qlen,
                                                3 * head_size * ggml_element_size(qkv), qkv->nb[1], 0);
        query_layer =
            ggml_rope_inplace(ctx->gctx.get(), query_layer, pos, rope_dim, 4, n_ctx); // [qlen, heads, head_size]
        query_layer = ggml_permute(ctx->gctx.get(), query_layer, 0, 2, 1, 3);            // [heads, qlen, head_size]

        ggml_tensor *key_layer =
            ggml_view_3d(ctx->gctx.get(), qkv, head_size, num_attention_heads, qlen, 3 * head_size * ggml_element_size(qkv),
                         qkv->nb[1], head_size * ggml_element_size(qkv));
        key_layer = ggml_rope_inplace(ctx->gctx.get(), key_layer, pos, rope_dim, 4, n_ctx); // [qlen, heads, head_size]
        key_layer = ggml_permute(ctx->gctx.get(), key_layer, 0, 2, 1, 3);                      // [heads, qlen, head_size]

        ggml_tensor *value_layer = ggml_view_3d(ctx->gctx.get(), qkv, head_size, num_attention_heads, qlen,
                                                3 * head_size * ggml_element_size(qkv), qkv->nb[1],
                                                2 * head_size * ggml_element_size(qkv)); // [qlen, heads, head_size]
        value_layer = ggml_permute(ctx->gctx.get(), value_layer, 1, 2, 0, 3);            // [heads, head_size, qlen]

        // store key & value to cache
        ggml_tensor *k_cache_view =
            ggml_view_3d(ctx->gctx.get(), k_cache, head_size, qlen, num_attention_heads, k_cache->nb[1], k_cache->nb[2],
                         n_past * head_size * ggml_element_size(k_cache)); // [heads, qlen, head_size]
        ggml_build_forward_expand(ctx->gf, ggml_cpy(ctx->gctx.get(), key_layer, k_cache_view));
        ggml_tensor *v_cache_view =
            ggml_view_3d(ctx->gctx.get(), v_cache, qlen, head_size, num_attention_heads, v_cache->nb[1], v_cache->nb[2],
                         n_past * ggml_element_size(v_cache)); // [heads, head_size, qlen]
        ggml_build_forward_expand(ctx->gf, ggml_cpy(ctx->gctx.get(), value_layer, v_cache_view));

        key_layer = ggml_view_3d(ctx->gctx.get(), k_cache, head_size, n_past + qlen, num_attention_heads, k_cache->nb[1],
                                 k_cache->nb[2], 0); // [heads, klen, head_size]
        value_layer = ggml_view_3d(ctx->gctx.get(), v_cache, n_past + qlen, head_size, num_attention_heads, v_cache->nb[1],
                                   v_cache->nb[2], 0); // [heads, head_size, klen]

        ggml_tensor *attn_scores = ggml_mul_mat(ctx->gctx.get(), key_layer, query_layer); // [heads, qlen, klen]
        if (n_past == 0)
        {
            // build attention mask for context input
            ggml_tensor *inf = ggml_new_tensor_3d(ctx->gctx.get(), attn_scores->type, 1, qlen - 1, num_attention_heads);
            ggml_set_f32(inf, -INFINITY);
            ggml_tensor *masked_attn_scores = ggml_view_3d(
                ctx->gctx.get(), attn_scores, 1, qlen - 1, num_attention_heads, qlen * ggml_element_size(attn_scores),
                qlen * qlen * ggml_element_size(attn_scores), (qlen - 1) * ggml_element_size(attn_scores));
            ggml_build_forward_expand(ctx->gf, ggml_cpy(ctx->gctx.get(), inf, masked_attn_scores));
        }
        attn_scores =
            ggml_scale_inplace(ctx->gctx.get(), attn_scores, 1.f / sqrtf((float)head_size));
        ggml_tensor *attn_probs = ggml_soft_max_inplace(ctx->gctx.get(), attn_scores); // [heads, qlen, klen]

        ggml_tensor *context_layer = ggml_mul_mat(ctx->gctx.get(), value_layer, attn_probs); // [heads, qlen, head_size]
        context_layer = ggml_reshape_2d(
            ctx->gctx.get(), ggml_cont(ctx->gctx.get(), ggml_permute(ctx->gctx.get(), context_layer, 0, 2, 1, 3)),
            hidden_size, qlen);

        ggml_tensor *attn_output = dense.forward(ctx, context_layer);
        return attn_output;
    }

    ggml_tensor *GLMBlock::forward(ForwardContext *ctx, ggml_tensor *hidden_states, int n_past)
    {
        float alpha = sqrtf(2.f * (float)num_hidden_layers);

        ggml_tensor *attn_input = input_layernorm.forward(ctx, hidden_states);
        ggml_tensor *attn_output = attention.forward(ctx, attn_input, n_past);
        ggml_build_forward_expand(ctx->gf, attn_output);
        hidden_states =
            ggml_add_inplace(ctx->gctx.get(), ggml_scale_inplace(ctx->gctx.get(), attn_input, alpha), attn_output);

        ggml_tensor *mlp_input = post_attention_layernorm.forward(ctx, hidden_states);
        ggml_tensor *mlp_output = mlp.forward(ctx, mlp_input);
        ggml_build_forward_expand(ctx->gf, mlp_output);
        ggml_tensor *output =
            ggml_add_inplace(ctx->gctx.get(), ggml_scale_inplace(ctx->gctx.get(), mlp_input, alpha), mlp_output);
        return output;
    }

    ggml_tensor *BaseConsolidatedQKVAttention::forward(ForwardContext *ctx, ggml_tensor *hidden_states, int n_past)
    {
        const int hidden_size = (int)hidden_states->ne[0];
        const int qlen = (int)hidden_states->ne[1];
        const int head_size = hidden_size / num_attention_heads;
        const int rope_dim = head_size / 2;
        const int mqa_scale = num_attention_heads / num_kv_heads;

        before_forward(ctx, n_past, qlen);

        ggml_tensor *qkv = query_key_value.forward(ctx, hidden_states); // [qlen, hidden + 2 * kv_hidden]

        ggml_tensor *tmpv =
            ggml_view_2d(ctx->gctx.get(), qkv, head_size * num_kv_heads, qlen,
                        qkv->nb[1],
                        head_size * (num_attention_heads + num_kv_heads) * ggml_element_size(qkv)); // [qlen, kv_hidden]

        ggml_tensor *key_layer =
            ggml_view_3d(ctx->gctx.get(), qkv, head_size, num_kv_heads, qlen, head_size * ggml_element_size(qkv),
                         qkv->nb[1], hidden_size * ggml_element_size(qkv)); // [qlen, kv_heads, head_size]

        ggml_tensor *query_layer =
            ggml_view_3d(ctx->gctx.get(), qkv, head_size, num_attention_heads, qlen, head_size * ggml_element_size(qkv),
                         qkv->nb[1], 0); // [qlen, heads, head_size]

        ggml_tensor *scores = cross_attention_3d(ctx, hidden_size, n_past, qlen, query_layer, key_layer, tmpv);

        ggml_tensor *attn_output = dense.forward(ctx, scores);
        return attn_output;
    }

    ggml_tensor *GLM2MLP::forward(ForwardContext *ctx, ggml_tensor *hidden_states)
    {
        ggml_tensor *output = dense_h_to_4h.forward(ctx, hidden_states);

        // swiglu activation
        ggml_tensor *x0 = ggml_view_2d(ctx->gctx.get(), output, output->ne[0] / 2, output->ne[1], output->nb[1], 0);
        ggml_tensor *x1 = ggml_view_2d(ctx->gctx.get(), output, output->ne[0] / 2, output->ne[1], output->nb[1],
                                       output->ne[0] / 2 * ggml_element_size(output));
        output = ggml_mul_inplace(ctx->gctx.get(), ggml_silu_inplace(ctx->gctx.get(), ggml_cont(ctx->gctx.get(), x0)), x1);

        output = dense_4h_to_h.forward(ctx, output);
        return output;
    }

    ggml_tensor *TheMLP::forward(ForwardContext *ctx, ggml_tensor *hidden_states)
    {
        ggml_tensor *intermediate = fc0.forward(ctx, hidden_states);
        intermediate = inplace_act(ctx->gctx.get(), act, intermediate);
        ggml_tensor *output = fc1.forward(ctx, intermediate);
        return output;
    }

    void TheMLP::set_prec(ggml_prec prec)
    {
        Block::set_prec(prec);
        fc0.set_prec(prec);
        fc1.set_prec(prec);
    }

    ggml_tensor *BaseMLP::forward(ForwardContext *ctx, ggml_tensor *hidden_states)
    {
        ggml_tensor *act = inplace_act(ctx->gctx.get(), this->act, gate_proj.forward(ctx, hidden_states));
        ggml_tensor *proj = up_proj.forward(ctx, hidden_states);

        ggml_tensor *output = ggml_mul_inplace(ctx->gctx.get(), act, proj);
        output = down_proj.forward(ctx, output);
        return output;
    }

    ggml_tensor *CoreAttention::calc_attn_scores(ForwardContext *ctx, int hidden_size, const int n_past, const int qlen,
        ggml_tensor *key_layer, ggml_tensor *query_layer, ggml_tensor *value_layer)
    {
        const int head_size = hidden_size / num_attention_heads;

        // note auto-broadcasting in ggml_mul_mat for `repeat > 1`
        ggml_tensor *attn_scores = ggml_mul_mat(ctx->gctx.get(), key_layer, query_layer); // [heads, qlen, klen]

        ggml_mul_mat_set_prec(attn_scores, prec);

        if (attn_scaling)
        {
            if (attn_scaling_factor > 0)
                attn_scores = ggml_scale_inplace(ctx->gctx.get(), attn_scores, attn_scaling_factor);
            else
                attn_scores = ggml_scale_inplace(ctx->gctx.get(), attn_scores, 1.f / sqrtf((float)head_size));
        }

        attn_scores = apply_pos_embedding_kq(ctx, attn_scores, hidden_size, qlen, pos);

        // attn_masked = mask_past(attn_scores)
        struct ggml_tensor * attn_masked = causal ? ggml_diag_mask_inf_inplace(ctx->gctx.get(), attn_scores, n_past)
                                                  : attn_scores;

        // attn_probs = soft_max(attn_masked)
        struct ggml_tensor * attn_probs = ggml_soft_max_inplace(ctx->gctx.get(), attn_masked);

        ggml_tensor *context_layer = ggml_mul_mat(ctx->gctx.get(), value_layer, attn_probs); // [heads, qlen, head_size]
        last_attn_scores = ggml_reshape_2d(
            ctx->gctx.get(),
            ggml_cont(ctx->gctx.get(), ggml_permute(ctx->gctx.get(), context_layer, 0, 2, 1, 3)),
            hidden_size, qlen);

        return last_attn_scores;
    }

    ggml_tensor *CoreAttention::cross_attention_after_pe(ForwardContext *ctx, const int hidden_size, const int n_past, const int qlen,
                                             ggml_tensor *query_layer, ggml_tensor *key_layer, ggml_tensor *v)
    {
        const int head_size = hidden_size / num_attention_heads;

        if (!attn_scaling)
            query_layer = ggml_scale(ctx->gctx.get(), query_layer, 1.f / sqrtf((float)head_size));

        // store key and value to memory
        save_to_cache(ctx, n_past, qlen, key_layer, v);

        query_layer = ggml_permute(ctx->gctx.get(), query_layer, 0, 2, 1, 3);                     // [heads, qlen, head_size]

        key_layer = get_k_from_cache(ctx, hidden_size, n_past, qlen);

        ggml_tensor * value_layer = get_v_from_cache(ctx, hidden_size, n_past, qlen);

        ggml_tensor *attn_scores = calc_attn_scores(ctx, hidden_size, n_past, qlen, key_layer, query_layer, value_layer);
        return attn_scores;
    }

    ggml_tensor *CoreAttention::cross_attention_3d(ForwardContext *ctx, const int hidden_size, const int n_past, const int qlen,
                                             ggml_tensor *query_layer, ggml_tensor *key_layer, ggml_tensor *v)
    {
        const int head_size = hidden_size / num_attention_heads;

        // [qlen, heads, head_size]
        key_layer = apply_pos_embedding_k(ctx, key_layer, hidden_size, qlen, pos);

        // [qlen, heads, head_size]
        query_layer = apply_pos_embedding_q(ctx, query_layer, hidden_size, qlen, pos);

        ggml_tensor *attn_scores = cross_attention_after_pe(ctx, hidden_size, n_past, qlen, query_layer, key_layer, v);

        return attn_scores;
    }

    ggml_tensor *CoreAttention::cross_attention(ForwardContext *ctx, const int hidden_size, const int n_past, const int qlen,
                                             ggml_tensor *q, ggml_tensor *k, ggml_tensor *v)
    {
        const int head_size = hidden_size / num_attention_heads;

        // [qlen, heads, head_size]
        ggml_tensor * key_layer = ggml_reshape_3d(ctx->gctx.get(), k, head_size, num_kv_heads, qlen);
        key_layer = apply_pos_embedding_k(ctx, key_layer, hidden_size, qlen, pos);

        // [qlen, heads, head_size]
        ggml_tensor * query_layer = ggml_reshape_3d(ctx->gctx.get(), q, head_size, num_attention_heads, qlen);
        query_layer = apply_pos_embedding_q(ctx, query_layer, hidden_size, qlen, pos);

        ggml_tensor *attn_scores = cross_attention_after_pe(ctx, hidden_size, n_past, qlen, query_layer, key_layer, v);

        return attn_scores;
    }

    void CoreAttention::before_forward(ForwardContext *ctx, const int n_past, const int qlen)
    {
        fill_pos_vector(pos, n_past, qlen);
    }

    void KVCacheAttention::before_forward(ForwardContext *ctx, const int n_past, const int qlen)
    {
        CoreAttention::before_forward(ctx, n_past, qlen);

        // shift cache
        if (shift_pending.shift > 0)
        {
            int remain = shift_pending.total - shift_pending.shift;
            if (remain > 0)
            {
                struct ggml_tensor * k_cache_remain = ggml_view_1d(ctx->gctx.get(), k_cache, remain * k_hidden_size,
                                            ggml_element_size(k_cache) * k_hidden_size * shift_pending.shift);
                struct ggml_tensor * k_cache_1d = ggml_view_1d(ctx->gctx.get(), k_cache, remain * k_hidden_size,
                                            0);

                struct ggml_tensor * v_cache_remain = ggml_view_2d(ctx->gctx.get(), v_cache, remain, v_hidden_size,
                                            cache_length * ggml_element_size(v_cache),
                                            shift_pending.shift * ggml_element_size(v_cache));
                struct ggml_tensor * v_cache_2d =     ggml_view_2d(ctx->gctx.get(), v_cache, remain, v_hidden_size,
                                            cache_length * ggml_element_size(v_cache),
                                            0);

                ggml_build_forward_expand(ctx->gf, ggml_cpy(ctx->gctx.get(), k_cache_remain, k_cache_1d));
                ggml_build_forward_expand(ctx->gf, ggml_cpy(ctx->gctx.get(), v_cache_remain, v_cache_2d));
            }
            shift_pending.clear();
        }
    }

    void KVCacheAttention::save_to_cache(ForwardContext *ctx, const int n_past, const int qlen,
        ggml_tensor *k, ggml_tensor *v)
    {
        // compute the transposed [N, n_embd] V matrix
        struct ggml_tensor * Vcur = ggml_transpose(ctx->gctx.get(), v); // ggml_reshape_2d(ctx->gctx.get(), tmpv, v_hidden_size, qlen));
        struct ggml_tensor * v_cache_view = ggml_view_2d(ctx->gctx.get(), v_cache, qlen, v_hidden_size,
                cache_length * ggml_element_size(v_cache), n_past * ggml_element_size(v_cache));

        ggml_build_forward_expand(ctx->gf, ggml_cpy(ctx->gctx.get(), Vcur, v_cache_view));

        struct ggml_tensor * k_cache_view = nullptr;
        struct ggml_tensor * k_view = nullptr;
        if (ggml_is_contiguous(k))
        {
            k_cache_view = ggml_view_1d(ctx->gctx.get(), k_cache, qlen * k_hidden_size,
                                    ggml_element_size(k_cache) * k_hidden_size * n_past);
            k_view = ggml_view_1d(ctx->gctx.get(), k, qlen * k_hidden_size, 0);
        }
        else
        {
            // [qlen, heads, head_size]
            const int head_size = k_hidden_size / num_kv_heads;
            k_view = k;
            k_cache_view = ggml_view_1d(ctx->gctx.get(), k_cache, qlen * k_hidden_size,
                                    ggml_element_size(k_cache) * k_hidden_size * n_past);
            k_cache_view = ggml_reshape_3d(ctx->gctx.get(), k_cache_view, head_size, num_kv_heads, qlen);  // [qlen, heads, head_size]
        }

        // important: storing RoPE-ed version of K in the KV cache!
        ggml_build_forward_expand(ctx->gf, ggml_cpy(ctx->gctx.get(), k_view, k_cache_view));

    }

    ggml_tensor *KVCacheAttention::get_k_from_cache(ForwardContext *ctx, const int hidden_size, const int n_past, const int qlen)
    {
        const int head_size = k_hidden_size / num_kv_heads;

        ggml_tensor *key_layer = nullptr;

        key_layer = ggml_view_1d(ctx->gctx.get(), k_cache, (n_past + qlen) * k_hidden_size, 0);
        key_layer = ggml_reshape_3d(ctx->gctx.get(), key_layer, head_size, num_kv_heads, n_past + qlen);  // [qlen, heads, head_size]
        key_layer = ggml_permute(ctx->gctx.get(), key_layer, 0, 2, 1, 3);                                 // [heads, qlen, head_size]

        return key_layer;
    }

    ggml_tensor *KVCacheAttention::get_v_from_cache(ForwardContext *ctx, const int hidden_size, const int n_past, const int qlen)
    {
        const int head_size = v_hidden_size / num_kv_heads;

        ggml_tensor * value_layer = ggml_view_3d(ctx->gctx.get(),
                        v_cache,
                        n_past + qlen, head_size, num_kv_heads,
                        cache_length * ggml_element_size(v_cache),
                        cache_length * ggml_element_size(v_cache) * head_size,
                        0); // [heads, head_size, klen]
        return value_layer;
    }

    ggml_tensor *BaseAttention::forward(ForwardContext *ctx, ggml_tensor *hidden_states, int n_past)
    {
        const int hidden_size = o_proj.in_features();
        const int qlen = (int)hidden_states->ne[1];

        before_forward(ctx, n_past, qlen);

        ggml_tensor *tmpq = q_proj.forward(ctx, hidden_states);
        ggml_tensor *tmpk = k_proj.forward(ctx, hidden_states);
        ggml_tensor *tmpv = v_proj.forward(ctx, hidden_states);

        ggml_mul_mat_set_prec(tmpk, prec);
        ggml_mul_mat_set_prec(tmpq, prec);
        ggml_mul_mat_set_prec(tmpv, prec);

        ggml_tensor *scores = cross_attention(ctx, hidden_size, n_past, qlen, tmpq, tmpk, tmpv);

        ggml_tensor *attn_output = o_proj.forward(ctx, scores);
        return attn_output;
    }

    void BaseCachelessAttention::save_to_cache(ForwardContext *ctx, const int n_past, const int qlen, ggml_tensor *k, ggml_tensor *v)
    {
        raw_k = k;
        raw_v = v;
    }

    ggml_tensor *BaseCachelessAttention::get_k_from_cache(ForwardContext *ctx, const int hidden_size, const int n_past, const int qlen)
    {
        // [qlen, heads, head_size] -> [heads, qlen, head_size]
        ggml_tensor *r = ggml_permute(ggctx, raw_k, 0, 2, 1, 3);
        return r;
    }

    ggml_tensor *BaseCachelessAttention::get_v_from_cache(ForwardContext *ctx, const int hidden_size, const int n_past, const int qlen)
    {
        const int head_size = hidden_size / num_attention_heads;

        // [qlen, hidden_size] -> [heads, head_size, qlen]
        ggml_tensor *r = ggml_reshape_3d(ggctx, raw_v, head_size, num_kv_heads, qlen);  // -> [qlen, heads, head_size]
        r = ggml_permute(ggctx, r, 1, 2, 0, 3);   // [heads, head_size, qlen]
        r = ggml_cont(ggctx, r);
        return r;
    }

    ggml_tensor *BaichuanSelfAttention::apply_pos_embedding_k(ForwardContext *ctx, ggml_tensor *k, int hidden_size, int qlen, ggml_tensor * past) const
    {
        return k;
    }

    ggml_tensor *BaichuanSelfAttention::apply_pos_embedding_q(ForwardContext *ctx, ggml_tensor *q, int hidden_size, int qlen, ggml_tensor * past) const
    {
        return q;
    }

    ggml_tensor *BaichuanSelfAttention::apply_pos_embedding_kq(ForwardContext *ctx, ggml_tensor *kq, int hidden_size, int qlen, ggml_tensor *past) const
    {
        const float max_alibi_bias = 8.0f;
        return ggml_alibi(ggctx, kq, /*n_past*/ 0, num_attention_heads, max_alibi_bias);
    }

    QWenSelfAttention::QWenSelfAttention(InitContext *ctx, int hidden_size, int num_attention_heads, int max_length)
        : RoPESelfAttention(ctx, hidden_size, num_attention_heads, max_length, true, false),
            seq_length(0),
            use_dynamic_ntk(false),
            use_logn_attn(false),
            logn_list(ggml_new_tensor_1d(ctx->gctx.get(), GGML_TYPE_F32, max_length))
    {
        logn_list->data = new char[ggml_nbytes(logn_list)];
    }

    void QWenSelfAttention::config(int rope_dim, float rope_freq_base, int seq_length, bool use_dynamic_ntk, bool use_logn_attn)
    {
        this->rope_dim = rope_dim;
        this->freq_base = rope_freq_base;
        this->seq_length = seq_length;
        this->use_dynamic_ntk = use_dynamic_ntk;
        this->use_logn_attn = use_logn_attn;

        if (use_logn_attn)
        {
            float *p = (float *)logn_list->data;
            for (int i = 0; i < max_length; i++)
                p[i] = i > seq_length ? logf(float(i)) / logf((float)seq_length) : 1.0f;
        }
    }

    ggml_tensor *QWenSelfAttention::apply_pos_embedding_k(ForwardContext *ctx, ggml_tensor *k, int hidden_size, int qlen, ggml_tensor * past) const
    {
        // [qlen, heads, head_size]
        return ggml_map_custom2(ggctx, k, past, ggml_compute_forward_ntk_dynamic_rope, GGML_N_TASKS_MAX, const_cast<QWenSelfAttention *>(this));
    }

    ggml_tensor *QWenSelfAttention::apply_pos_embedding_q(ForwardContext *ctx, ggml_tensor *q, int hidden_size, int qlen, ggml_tensor * past) const
    {
        // [qlen, heads, head_size];
        ggml_tensor *r = ggml_map_custom2(ggctx, q, past, ggml_compute_forward_ntk_dynamic_rope, GGML_N_TASKS_MAX, const_cast<QWenSelfAttention *>(this));
        if (use_logn_attn)
        {
            const int *p = (const int *)past->data;
            int last_n = p[qlen - 1];
            if (last_n > seq_length)
            {
                ggml_tensor *scale = ggml_view_1d(ggctx, logn_list, qlen, p[0] * ggml_element_size(logn_list));
                r = ggml_map_custom2(ggctx, r, scale, ggml_compute_forward_mat_scale, GGML_N_TASKS_MAX, nullptr);
            }
        }
        return r;
    }

    void BlueLMSelfAttention::config(float rope_theta, float rope_scaling_factor, float rope_scaling_power)
    {
        this->freq_base = rope_theta;
        this->rope_scaling_factor = rope_scaling_factor;
        this->rope_scaling_power = rope_scaling_power;
    }

    void BlueLMSelfAttention::build_inv_freq_if_needed(int hidden_size)
    {
        if (cached_hidden_size != hidden_size)
        {
            cached_hidden_size = hidden_size;
            build_ntk_mixed_inv_freq(rope_dim, inv_freq, (int)((float)max_length / rope_scaling_factor), freq_base, rope_scaling_factor, rope_scaling_power);
        }
    }

    ggml_tensor *BlueLMSelfAttention::apply_pos_embedding_k(ForwardContext *ctx, ggml_tensor *k, int hidden_size, int qlen, ggml_tensor * past) const
    {
        const_cast<BlueLMSelfAttention *>(this)->rope_dim = hidden_size / num_attention_heads;
        if (rope_scaling_power > 0.0)
        {
            const_cast<BlueLMSelfAttention *>(this)->build_inv_freq_if_needed(hidden_size);
            return ggml_map_custom2(ggctx, k, past, ggml_compute_forward_ntk_mix_rope, GGML_N_TASKS_MAX, const_cast<BlueLMSelfAttention *>(this));
        }
        else
            return RoPESelfAttention::apply_pos_embedding_k(ctx, k, hidden_size, qlen, past);
    }

    ggml_tensor *BlueLMSelfAttention::apply_pos_embedding_q(ForwardContext *ctx, ggml_tensor *q, int hidden_size, int qlen, ggml_tensor * past) const
    {
        const_cast<BlueLMSelfAttention *>(this)->rope_dim = hidden_size / num_attention_heads;
        if (rope_scaling_power > 0.0)
        {
            const_cast<BlueLMSelfAttention *>(this)->build_inv_freq_if_needed(hidden_size);
            return ggml_map_custom2(ggctx, q, past, ggml_compute_forward_ntk_mix_rope, GGML_N_TASKS_MAX, const_cast<BlueLMSelfAttention *>(this));
        }
        else
            return RoPESelfAttention::apply_pos_embedding_q(ctx, q, hidden_size, qlen, past);
    }

    ggml_tensor *RobertaBlock::forward(ForwardContext *ctx, ggml_tensor *hidden_states, int n_past)
    {
        // CAUTION: MEMORY REUSED BETWEEN LAYERS

        ggml_tensor *attn_outputs = attention.forward(ctx, hidden_states, n_past);

        // see XLMRobertaSelfOutput
        ggml_tensor *sum = ggml_add(ggctx, hidden_states, attn_outputs);
        ggml_tensor *attention_output = post_attention_layernorm.forward(ctx, sum);

        ggml_tensor *r = mlp.forward(ctx, attention_output);
        return r;
    }

    ggml_tensor *RobertaOutput::forward(ForwardContext *ctx, ggml_tensor *hidden_states, ggml_tensor *attention_output)
    {
        ggml_tensor *r = dense.forward(ctx, hidden_states);
        r = ggml_add_inplace(ggctx, r, attention_output);
        r = norm.forward(ctx, r);
        return r;
    }

    ggml_tensor *RobertaMLP::forward(ForwardContext *ctx, ggml_tensor *hidden_states)
    {
        ggml_tensor *temp = intermediate.forward(ctx, hidden_states);
        temp = inplace_act(ctx->gctx.get(), act, temp);
        temp = output.forward(ctx, temp, hidden_states);
        return temp;
    }

    ggml_tensor *FuyuEmbedding::forward(ForwardContext *ctx, ggml_tensor *patches, int patches_per_row, ggml_tensor *text_input)
    {
        //ggml_get_rows
        return nullptr;
    }

    static void build_inv_freq_from_factors(std::vector<float> &inv_freq, int dim, const float *factors, float base)
    {
        inv_freq.clear();
        inv_freq.reserve(dim / 2);
        for (int i = 0; i < dim; i += 2)
        {
            double v = 1.0 / (factors[i / 2] * pow(base, (double)i / dim));
            inv_freq.push_back((float)v);
        }
    }

    void Phi3SUSelfAttention::config(int original_max_position_embeddings, float rope_theta, float scaling_factor, int factor_len, const float *short_factor, const float *long_factor)
    {
        this->original_max_position_embeddings = original_max_position_embeddings;
        this->freq_base = rope_theta;
        this->scaling_factor = scaling_factor;
        build_inv_freq_from_factors(this->inv_freq_short, factor_len * 2, short_factor, freq_base);
        build_inv_freq_from_factors(this->inv_freq_long,  factor_len * 2, long_factor,  freq_base);
    }

    const float *Phi3SUSelfAttention::get_inv_freq(int pos)
    {
        // This does not work.
        // pos > original_max_position_embeddings ? inv_freq_long.data() : inv_freq_short.data();
        return max_length > original_max_position_embeddings ? inv_freq_long.data() : inv_freq_short.data();
    }

    ggml_tensor *Phi3SUSelfAttention::apply_pos_embedding_k(ForwardContext *ctx, ggml_tensor *k, int hidden_size, int qlen, ggml_tensor * past) const
    {
        return ggml_map_custom2(ggctx, k, past, ggml_compute_forward_su_rope, GGML_N_TASKS_MAX, const_cast<Phi3SUSelfAttention *>(this));
    }

    ggml_tensor *Phi3SUSelfAttention::apply_pos_embedding_q(ForwardContext *ctx, ggml_tensor *q, int hidden_size, int qlen, ggml_tensor * past) const
    {
        return ggml_map_custom2(ggctx, q, past, ggml_compute_forward_su_rope, GGML_N_TASKS_MAX, const_cast<Phi3SUSelfAttention *>(this));
    }
}
