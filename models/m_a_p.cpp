namespace neo
{
    typedef yi::Config Config;

    class ChatHistoryEncoder : public BaseHistoryEncoder
    {
    public:
        void append_pair(int round_idx, const std::string &user, const std::string &ai, std::vector<int> &ids) const override
        {
            auto *tok = tokenizer;
            append_user(round_idx, user, ids);

            tok->encode(ai, ids);
            ids.push_back(tok->eos_token_id);
        }

        void append_sys_prompt(std::vector<int> &ids) const override
        {
            auto *tok = tokenizer;
            if (tok->get_system_prompt().size() > 0)
            {
                std::ostringstream oss;
                oss << "<<SYS>>\n" << tok->get_system_prompt() << "\n<</SYS>>";
                tok->encode(oss.str(), ids);
            }
        }

        void do_append_user(int round_idx, const std::string &user, std::vector<int> &ids) const override
        {
            auto *tok = tokenizer;

            std::ostringstream oss;
            oss << "[INST] " << user << " [/INST]";
            ids.push_back(tok->bos_token_id);
            tok->encode(oss.str(), ids);
        }
    };

    static ChatHistoryEncoder _chat_encoder;

    class Tokenizer : public llama::v2::Tokenizer
    {
    public:
        Tokenizer(const Config &config)
            : llama::v2::Tokenizer::Tokenizer(config, &_chat_encoder)
        {
            sys_prompt = "You are a helpful, respectful and honest assistant named Neo.";
        }

        size_t load(const char *buffer, int n_vocab) override
        {
            tp = new tokenizer::BPEProcessor1();
            size_t size = tp->Load(buffer, n_vocab);
            return size;
        }
    };

    class ConditionalGeneration : public yi::ConditionalGeneration
    {
    public:
        ConditionalGeneration() = default;
        ConditionalGeneration(const Config &config)
            : yi::ConditionalGeneration(config, ModelType::MODEL_TYPE_MAP_NEO)
        {}
    };
}