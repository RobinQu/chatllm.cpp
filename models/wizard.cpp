namespace lm
{
    struct Config : public llama::v2::Config
    {
    };

    class ChatHistoryEncoder : public BaseHistoryEncoder
    {
    public:
        void append_sys_prompt(std::vector<int> &ids) const override;
        void append_pair(int round_idx, const std::string &user, const std::string &ai, std::vector<int> &ids) const override;
        void do_append_user(int round_idx, const std::string &user, std::vector<int> &ids) const override;
    };

    static ChatHistoryEncoder _chat_encoder;

    class Tokenizer : public llama::v2::Tokenizer
    {
    public:
        Tokenizer(const Config &config)
            : llama::v2::Tokenizer::Tokenizer(config, &_chat_encoder)
        {
            sys_prompt = "A chat between a curious user and an artificial intelligence assistant. The assistant gives helpful, detailed, and polite answers to the user's questions. USER: Hi ASSISTANT: Hello.";
        }
    };

    class ConditionalGeneration : public llama::v2::ConditionalGeneration
    {
    public:
        ConditionalGeneration() = default;
        ConditionalGeneration(const Config &config)
            : llama::v2::ConditionalGeneration(config, MODEL_TYPE_WIZARDLM)
        {
        }
    };

    void ChatHistoryEncoder::append_pair(int round_idx, const std::string &user, const std::string &ai, std::vector<int> &ids) const
    {
        Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);
        append_user(round_idx, user, ids);
        tok->encode(ai, ids, false, true);
    }

    void ChatHistoryEncoder::append_sys_prompt(std::vector<int> &ids) const
    {
        Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);
        std::ostringstream oss_prompt;

        if (tok->get_system_prompt().size() > 0)
        {
            oss_prompt << tok->get_system_prompt() << " ";
            auto text = oss_prompt.str();
            tok->encode(text, ids, false, false);
        }
    }

    void ChatHistoryEncoder::do_append_user(int round_idx, const std::string &user, std::vector<int> &ids) const
    {
        Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);
        std::ostringstream oss_prompt;

        oss_prompt << "USER: " + user << " ASSISTANT:";

        auto text = oss_prompt.str();
        tok->encode(text, ids, false, false);
    }
}

namespace coder
{
    struct Config : public codellama::Config
    {
    };

    class ChatHistoryEncoder : public BaseHistoryEncoder
    {
    public:
        void append_sys_prompt(std::vector<int> &ids) const override;
        void append_pair(int round_idx, const std::string &user, const std::string &ai, std::vector<int> &ids) const override;
        void do_append_user(int round_idx, const std::string &user, std::vector<int> &ids) const override;
    };

    static ChatHistoryEncoder _chat_encoder;

    class Tokenizer : public llama::v2::Tokenizer
    {
    public:
        Tokenizer(const llama::v2::Config &config)
            : llama::v2::Tokenizer::Tokenizer(config, &_chat_encoder)
        {
            sys_prompt = "Below is an instruction that describes a task. Write a response that appropriately completes the request.";
        }
    };

    class ConditionalGeneration : public codellama::ConditionalGeneration
    {
    public:
        ConditionalGeneration() = default;

        ConditionalGeneration(const Config &config, ModelType type)
            : codellama::ConditionalGeneration(config, type)
        {}

        ConditionalGeneration(const Config &config)
            : ConditionalGeneration(config, MODEL_TYPE_WIZARDCODER)
        {
        }
    };

    void ChatHistoryEncoder::append_pair(int round_idx, const std::string &user, const std::string &ai, std::vector<int> &ids) const
    {
        Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);
        std::ostringstream oss_prompt;

        oss_prompt << ai << "\n\n";

        auto text = oss_prompt.str();

        append_user(round_idx, user, ids);
        tok->encode(text, ids, false, false);
    }

    void ChatHistoryEncoder::append_sys_prompt(std::vector<int> &ids) const
    {
        Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);
        std::ostringstream oss_prompt;

        if (tok->get_system_prompt().size() > 0)
        {
            oss_prompt << tok->get_system_prompt() << "\n\n";
            auto text = oss_prompt.str();
            tok->encode(text, ids, true, false);
        }
    }

    void ChatHistoryEncoder::do_append_user(int round_idx, const std::string &user, std::vector<int> &ids) const
    {
        Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);
        std::ostringstream oss_prompt;

        oss_prompt << "### Instruction:\n" + user << "\n\n### Response:\n";

        auto text = oss_prompt.str();
        tok->encode(text, ids, true, false);
    }
}

namespace math
{
    struct Config : public mistral::mistral::Config
    {
    };

    class Tokenizer : public coder::Tokenizer
    {
    public:
        Tokenizer(const llama::v2::Config &config)
            : coder::Tokenizer(config)
        {}
    };

    class ConditionalGeneration : public mistral::mistral::ConditionalGeneration
    {
    public:
        ConditionalGeneration(const Config &config)
            : mistral::mistral::ConditionalGeneration(config, MODEL_TYPE_WIZARDMATH)
        {
        }
    };
}

namespace moe
{
    typedef mistral::mixtral::Config Config;

    class Tokenizer : public mistral::mixtral::Tokenizer
    {
    public:
        Tokenizer(const Config &config)
            : mistral::mixtral::Tokenizer(config, &lm::_chat_encoder)
        {
            sys_prompt = "A chat between a curious user and an artificial intelligence assistant. The assistant gives helpful, detailed, and polite answers to the user's questions. ";
        }
    };

const int NUM_EXPERTS                   =  8;
const int EXPERTS_PER_TOK               =  2;

typedef mistral::mixtral::_ConditionalGeneration<NUM_EXPERTS, EXPERTS_PER_TOK, MODEL_TYPE_WIZARDLM2_MOE> ConditionalGeneration;
}