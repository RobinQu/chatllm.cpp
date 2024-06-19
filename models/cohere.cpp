namespace command_r
{
struct Config : public BaseConfig
{
    int num_key_value_heads;
    float rope_theta;
    float logit_scale;
};

class ChatHistoryEncoder : public BaseHistoryEncoder
{
public:
    void append_sys_prompt(std::vector<int> &ids) const override;
    void append_pair(int round_idx, const std::string &user, const std::string &ai, std::vector<int> &ids) const override;
    void do_append_user(int round_idx, const std::string &user, std::vector<int> &ids) const override;
};

static ChatHistoryEncoder _chat_encoder;

class Tokenizer : public BaseTokenizer
{
public:
    Tokenizer(const Config &config)
        : Tokenizer(config, &_chat_encoder)
    {}

    Tokenizer(const Config &config, BaseHistoryEncoder *encoder)
        : BaseTokenizer(config, encoder)
    {
        sys_prompt = R"""(You are a powerful conversational AI trained by Cohere to help people.)""";
    }

    size_t load(const char *buffer, int n_vocab) override
    {
        tp = new tokenizer::BPEProcessor2();
        size_t size = tp->Load(buffer, n_vocab);

        start_of_turn_token_id  = tp->PieceToId("<|START_OF_TURN_TOKEN|>");
        end_of_turn_token_id    = tp->PieceToId("<|END_OF_TURN_TOKEN|>");
        user_token_id           = tp->PieceToId("<|USER_TOKEN|>");
        chatbot_token_id        = tp->PieceToId("<|CHATBOT_TOKEN|>");
        system_token_id         = tp->PieceToId("<|SYSTEM_TOKEN|>");
        terminate_ids.insert(end_of_turn_token_id);
        return size;
    }

public:
    void encode(const std::string &text, std::vector<int> &ids, bool add_start, int start_token, bool add_end)
    {
        if (add_start)
        {
            ids.push_back(start_of_turn_token_id);
            if (start_token >= 0)
                ids.push_back(start_token);
        }
        BaseTokenizer::encode(text, ids);
        if (add_end)
        {
            ids.push_back(end_of_turn_token_id);
        }
    }

public:
    int start_of_turn_token_id;
    int end_of_turn_token_id;
    int user_token_id;
    int chatbot_token_id;
    int system_token_id;
};

class ConditionalGeneration : public BaseModelForConditionalGeneration<
                                  Model<BaseConfig, Embedding, LayerNormNoBias, CohereBlock, int, int, int, int, int>>
{
public:
    ConditionalGeneration(const Config &config, ModelType type = MODEL_TYPE_COHERE_COMMAND_R)
        : BaseModelForConditionalGeneration<
                                  Model<BaseConfig, Embedding, LayerNormNoBias, CohereBlock, int, int, int, int, int>>(type, config, MEM_SIZE, SCRATCH_SIZE), config(config)
    {
        constexpr size_t tensor_ovhd = GGML_TENSOR_SIZE + GGML_OBJECT_SIZE;
        const size_t num_tensors = 2 + config.num_hidden_layers * 11;
        const size_t ctx_size = num_tensors * tensor_ovhd;
        w_ctx_.gctx = GGMLContext({.mem_size = ctx_size, .mem_buffer = nullptr, .no_alloc = true});
        w_ctx_.dtype = config.dtype;

        transformer = new Model<BaseConfig, Embedding, LayerNormNoBias, CohereBlock, int, int, int, int, int>(&w_ctx_, config,
                nullptr,
                config.hidden_size, config.num_attention_heads,
                config.intermediate_size, config.num_key_value_heads, config.max_length);

        for (int i = 0; i < config.num_hidden_layers; i++)
        {
            auto &attention = transformer->layers[i].attention;
            attention.freq_base = config.rope_theta;
        }

        logit_scale = config.logit_scale;

        GRAPH_SIZE = 4096;
    }

    void load(ModelLoader &loader) override
    {
        loader.read_tensor("model.embed_tokens.weight", transformer->word_embeddings.weight);
        for (int i = 0; i < config.num_hidden_layers; i++)
        {
            std::string layer_prefix = "model.layers." + std::to_string(layer_ids[i]) + '.';
            loader.read_tensor(layer_prefix + "input_layernorm.weight", transformer->layers[i].input_layernorm.weight);
            loader.read_tensor(layer_prefix + "mlp.down_proj.weight", transformer->layers[i].mlp.down_proj.weight);
            loader.read_tensor(layer_prefix + "mlp.gate_proj.weight", transformer->layers[i].mlp.gate_proj.weight);
            loader.read_tensor(layer_prefix + "mlp.up_proj.weight", transformer->layers[i].mlp.up_proj.weight);

            loader.read_tensor(layer_prefix + "self_attn.k_proj.weight", transformer->layers[i].attention.k_proj.weight);
            loader.read_tensor(layer_prefix + "self_attn.o_proj.weight", transformer->layers[i].attention.o_proj.weight);
            loader.read_tensor(layer_prefix + "self_attn.q_proj.weight", transformer->layers[i].attention.q_proj.weight);
            loader.read_tensor(layer_prefix + "self_attn.v_proj.weight", transformer->layers[i].attention.v_proj.weight);
        }
        loader.read_tensor("model.norm.weight", transformer->final_layernorm.weight);

        CHATLLM_CHECK(ggml_used_mem(w_ctx_.gctx.get()) == ggml_get_mem_size(w_ctx_.gctx.get()))
            << "corrupted model weights";
    }

public:
    static constexpr size_t MEM_SIZE = 2048ull * 1024 * 1024;
    static constexpr size_t SCRATCH_SIZE = 1024ull * 1024 * 1024;

    BaseConfig config;

private:
    // hold ggml_context & kv_cache
    InitContext w_ctx_; // weight context
};

void ChatHistoryEncoder::append_pair(int round_idx, const std::string &user, const std::string &ai, std::vector<int> &ids) const
{
    Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);
    append_user(round_idx, user, ids);

    tok->encode(ai, ids, false, -1, true);
}

void ChatHistoryEncoder::append_sys_prompt(std::vector<int> &ids) const
{
    Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);

    ids.push_back(tok->bos_token_id);
    if (tok->get_system_prompt().size() > 0)
    {
        tok->encode(tok->get_system_prompt(), ids, true, tok->system_token_id, true);
    }
}

void ChatHistoryEncoder::do_append_user(int round_idx, const std::string &user, std::vector<int> &ids) const
{
    Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);

    tok->encode(user, ids, true, tok->user_token_id, true);
    ids.push_back(tok->start_of_turn_token_id);
    ids.push_back(tok->chatbot_token_id);
}

}

namespace aya_23
{
typedef command_r::Config Config;

typedef command_r::Tokenizer Tokenizer;

class ConditionalGeneration : public command_r::ConditionalGeneration
{
public:
    ConditionalGeneration(const Config &config) : command_r::ConditionalGeneration(config, MODEL_TYPE_COHERE_AYA_23) {}
};
}