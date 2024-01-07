namespace _7b
{
    struct Config : public llama::Config
    {
        int user_token_id;
        int assistant_token_id;
    };

    class ChatHistoryEncoder : public BaseHistoryEncoder
    {
    public:
        void append_pair(int round_idx, const std::string &user, const std::string &ai, std::vector<int> &ids) const override;
        void append_user(int round_idx, const std::string &user, std::vector<int> &ids) const override;
    };

    static ChatHistoryEncoder _chat_encoder;

    class Tokenizer : public llama::Tokenizer
    {
    public:
        Tokenizer(const Config &config)
            : llama::Tokenizer::Tokenizer(config, &_chat_encoder),
            user_token_id(config.user_token_id),
            assistant_token_id(config.assistant_token_id)
        {
        }

        size_t load(const char *buffer, int n_vocab) override;

        bool is_special_id(int id) const override;

    public:
        int user_token_id;
        int assistant_token_id;
    };

    class ConditionalGeneration : public llama::ConditionalGeneration
    {
    public:
        ConditionalGeneration() = default;
        ConditionalGeneration(const Config &config)
            : llama::ConditionalGeneration(config, MODEL_TYPE_BAICHUANLLAMA)
        {
        }
    };

    size_t Tokenizer::load(const char *buffer, int n_vocab)
    {
        tp = new tokenizer::SentencePieceProcessor();
        size_t size = tp->Load(buffer, n_vocab);
        return size;
    }

    void ChatHistoryEncoder::append_pair(int round_idx, const std::string &user, const std::string &ai, std::vector<int> &ids) const
    {
        Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);

        if ((round_idx == 0) && (tok->get_system_prompt().size() > 0))
            tok->encode(tok->get_system_prompt(), ids, false, false);

        ids.push_back(tok->user_token_id);
        tok->encode(user, ids, false, false);
        ids.push_back(tok->assistant_token_id);
        tok->encode(user, ids, false, false);
    }

    void ChatHistoryEncoder::append_user(int round_idx, const std::string &user, std::vector<int> &ids) const
    {
        Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);

        if ((round_idx == 0) && (tok->get_system_prompt().size() > 0))
            tok->encode(tok->get_system_prompt(), ids, false, false);

        ids.push_back(tok->user_token_id);
        tok->encode(user, ids, false, false);
        ids.push_back(tok->assistant_token_id);
    }

    bool Tokenizer::is_special_id(int id) const
    {
        return (id == pad_token_id) || (id == user_token_id) || (id == assistant_token_id);
    }
}

namespace larger
{
    typedef _7b::Config Config;
    typedef _7b::Tokenizer Tokenizer;

    class ConditionalGeneration : public llama::GenericConditionalGeneration<BaichuanBlock>
    {
    public:
        ConditionalGeneration() = default;
        ConditionalGeneration(const Config &config, ModelType type = ModelType::MODEL_TYPE_BAICHUAN)
            : ConditionalGeneration(config, type, config.num_attention_heads, config.max_length)
        {};

        ConditionalGeneration(const Config &config, ModelType type,
                            int num_key_value_heads, int max_length)
            : llama::GenericConditionalGeneration<BaichuanBlock>(config, type, num_key_value_heads, max_length)
        {}
    };
}