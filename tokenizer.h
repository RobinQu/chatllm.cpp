#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <memory>

namespace tokenizer
{

enum token_type
{
    UNDEFINED    = 0,
    NORMAL       = 1,
    UNKNOWN      = 2,
    CONTROL      = 3,
    USER_DEFINED = 4,
    UNUSED       = 5,
    BYTE         = 6,
};

struct _vocab
{
    using id    = int32_t;
    using token = std::string;

    struct token_score {
        token tok;
        float score;
        token_type type;
    };

    std::unordered_map<token, id> token_to_id;
    std::vector<token_score> id_to_token;

    std::unordered_map<id, token> special_tokens_cache;
    std::map<std::pair<std::string, std::string>, int> bpe_ranks;

    int find_bpe_rank(std::string token_left, std::string token_right) const
    {
        auto it = bpe_ranks.find(std::make_pair(token_left, token_right));
        if (it == bpe_ranks.end()) {
            return -1;
        }

        return it->second;
    }

    bool is_token_of_type(id id, token_type t) const
    {
        if ((id < 0) || (id >= (int)id_to_token.size())) return false;
        return t == id_to_token[id].type;
    }

    bool is_normal_token(id id) const
    {
        return is_token_of_type(id, token_type::NORMAL);
    }

    bool is_control_token(id id) const
    {
        return is_token_of_type(id, token_type::CONTROL);
    }
};

class TextPreprocessor
{
public:
    virtual ~TextPreprocessor() {}
    virtual std::string transform(const std::string &s) = 0;
};

class TextPrepTrim : public TextPreprocessor
{
public:
    std::string transform(const std::string &s) override;
};

class TextTrim : public TextPreprocessor
{
public:
    std::string transform(const std::string &s) override;
};

class TextPrepDeleteMultiSpaces : public TextPreprocessor
{
public:
    std::string transform(const std::string &s) override;
};

class TextPrepNewlineToSpaces : public TextPreprocessor
{
public:
    std::string transform(const std::string &s) override;
};

class TextPrepAddLeadingSpace : public TextPreprocessor
{
public:
    std::string transform(const std::string &s) override;
};

class Processor
{
public:
    struct TokenId
    {
        std::string token;
        int id;
    };

    Processor() : piece_size(0), id_unk_token(-1), token_unk_id("<?>"), ret_special_token(false) {}

    virtual size_t Load(const char *buffer, int n_vocab) = 0;

    virtual int PieceToId(std::string_view piece) const;

    virtual const std::string IdToPiece(int id) const;

    virtual int Encode(const std::string &input,
            std::vector<std::string> *pieces) const;

    // Given a UTF8 input, encodes it into a sequence of ids.
    virtual int Encode(const std::string &input,
            std::vector<int> *ids) const;

    // Given a sequence of ids, decodes it into a detokenized output.
    virtual int Decode(const std::vector<int> &ids,
            std::string *detokenized) const;

    int GetPieceSize(void) const { return piece_size; }

    void SetIdUnkownToken(int id) { id_unk_token = id; }

    void SetTokenUnknownId(const std::string &s) { token_unk_id = s; }

    void EnableReturnSpecialToken(bool en) { ret_special_token = en; }

    void RegisterPreprocessor(TextPreprocessor* prep);

    void OverrideTokenDecoding(int id, const std::string &tok);

    void AddAddedToken(const std::string &tok, int id);

protected:
    virtual int DoEncode(const std::string &input, std::vector<int> *ids) const = 0;

protected:
    _vocab vocab_;
    int piece_size;
    int id_unk_token;
    std::string token_unk_id;
    bool ret_special_token;
    std::vector<std::unique_ptr<TextPreprocessor>> pp;
    std::map<int, std::string> token_override;
    std::vector<TokenId> added_tokens;
};

class BPEProcessor1: public Processor
{
public:
    BPEProcessor1() : Processor::Processor()  {}

    size_t Load(const char *buffer, int n_vocab) override;

protected:
    int DoEncode(const std::string &input,
            std::vector<int> *ids) const override;
};

class BPEProcessor2: public Processor
{
public:
    BPEProcessor2() : Processor::Processor() {}

    size_t Load(const char *buffer, int n_vocab) override;

    const std::string IdToPiece(int id) const override;

protected:
    int DoEncode(const std::string &input,
            std::vector<int> *ids) const override;

    virtual int DoEncode2(const std::string &input,
            std::vector<int> *ids) const;
};

class BPEProcessor3: public BPEProcessor2
{
public:
    BPEProcessor3() : BPEProcessor2() {}

protected:
    int DoEncode2(const std::string &input,
            std::vector<int> *ids) const override;
};

class UnigramProcessor: public Processor
{
public:
    UnigramProcessor(int unk_tok_id);

    size_t Load(const char *buffer, int n_vocab) override;

    int unk_tok_id;

private:
    int DoEncode(const std::string &input,
            std::vector<int> *ids) const override;

private:
    size_t tok_max_len;
};

size_t get_end_of_valid_utf8(const std::string &utf8, const size_t offset);
}
