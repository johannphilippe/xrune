#pragma once
#include <string>

// Galdr lexical tokens. See dev/dsl_specification.md §2 and Appendix A.

namespace xrune::galdr {

enum class Tok {
    End,        // end of input
    Newline,    // significant statement terminator (suppressed inside brackets /
                // after a continuation operator)

    Ident,
    Number,
    String,

    // keywords
    KwRune, KwSigil, KwEnd, KwInput, KwOut, KwAs, KwTrue, KwFalse,

    // punctuation / brackets
    LParen, RParen, LBracket, RBracket,
    Assign,     // =
    Comma,      // ,
    Dot,        // .

    // connection algebra
    Colon,      // :   sequential
    Split,      // <:  split
    Merge,      // :>  merge
    Modulate,   // ~>  audio-rate modulation into a port
    Arrow,      // ->  explicit wire

    // arithmetic (compile-time, on numbers)
    Plus, Minus, Star, Slash, Percent,

    Tilde,      // ~   reserved (feedback), currently an error
    Unknown,
};

struct Token {
    Tok kind = Tok::Unknown;
    std::string text;   // identifier / keyword / string / raw number text
    double number = 0;  // parsed value when kind == Number
    int line = 1;
    int col = 1;
};

inline const char* tok_name(Tok k) {
    switch (k) {
        case Tok::End: return "end-of-input";
        case Tok::Newline: return "newline";
        case Tok::Ident: return "identifier";
        case Tok::Number: return "number";
        case Tok::String: return "string";
        case Tok::KwRune: return "'rune'";
        case Tok::KwSigil: return "'sigil'";
        case Tok::KwEnd: return "'end'";
        case Tok::KwInput: return "'input'";
        case Tok::KwOut: return "'out'";
        case Tok::KwAs: return "'as'";
        case Tok::KwTrue: return "'true'";
        case Tok::KwFalse: return "'false'";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::Assign: return "'='";
        case Tok::Comma: return "','";
        case Tok::Dot: return "'.'";
        case Tok::Colon: return "':'";
        case Tok::Split: return "'<:'";
        case Tok::Merge: return "':>'";
        case Tok::Modulate: return "'~>'";
        case Tok::Arrow: return "'->'";
        case Tok::Plus: return "'+'";
        case Tok::Minus: return "'-'";
        case Tok::Star: return "'*'";
        case Tok::Slash: return "'/'";
        case Tok::Percent: return "'%'";
        case Tok::Tilde: return "'~'";
        default: return "unknown token";
    }
}

// Operators after which a newline is a line continuation (spec §2).
inline bool continues_line(Tok k) {
    switch (k) {
        case Tok::LParen: case Tok::LBracket:
        case Tok::Assign: case Tok::Comma: case Tok::Dot:
        case Tok::Colon: case Tok::Split: case Tok::Merge:
        case Tok::Modulate: case Tok::Arrow:
        case Tok::Plus: case Tok::Minus: case Tok::Star:
        case Tok::Slash: case Tok::Percent: case Tok::Tilde:
            return true;
        default:
            return false;
    }
}

} // namespace xrune::galdr
