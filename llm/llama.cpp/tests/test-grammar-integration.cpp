#ifdef NDEBUG
#undef NDEBUG
#endif

#define LLAMA_API_INTERNAL

#include "ggml.h"
#include "llama.h"
#include "grammar-parser.h"
#include "json-schema-to-grammar.h"
#include "unicode.h"
#include <cassert>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

//#define INCLUDE_FAILING_TESTS 1

static llama_grammar* build_grammar(const std::string & grammar_str) {
    auto parsed_grammar = grammar_parser::parse(grammar_str.c_str());

    // Ensure we parsed correctly
    assert(!parsed_grammar.rules.empty());

    // Ensure we have a root node
    assert(!(parsed_grammar.symbol_ids.find("root") == parsed_grammar.symbol_ids.end()));

    std::vector<const llama_grammar_element*> grammar_rules(parsed_grammar.c_rules());
    llama_grammar* grammar = llama_grammar_init(
        grammar_rules.data(), grammar_rules.size(), parsed_grammar.symbol_ids.at("root"));

    return grammar;
}

static bool test_build_grammar_fails(const std::string & grammar_str) {
    fprintf(stderr, "⚫ Testing failure for grammar: %s\n", grammar_str.c_str());
    bool grammar_fails = false;
    try {
        build_grammar(grammar_str);
        fprintf(stderr, "  ❌ Expected build failure, but succeeded\n");
    } catch (const std::exception & err) {
        grammar_fails = true;
        fprintf(stdout, "  ✅︎\n");
    }
    return grammar_fails;
}

static bool match_string(const std::string & input, llama_grammar* grammar) {
    auto decoded = decode_utf8(input, {});

    const auto & code_points = decoded.first;

    for (auto it = code_points.begin(), end = code_points.end() - 1; it != end; ++it) {
        auto prev_stacks = grammar->stacks;
        llama_grammar_accept(grammar->rules, prev_stacks, *it, grammar->stacks);
        if (grammar->stacks.empty()) {
            // no stacks means that the grammar failed to match at this point
            return false;
        }
    }

    for (const auto & stack : grammar->stacks) {
        if (stack.empty()) {
            // An empty stack means that the grammar has been completed
            return true;
        }
    }

    return false;
}

static void test(const std::string & test_desc, const std::string & grammar_str, const std::vector<std::string> & passing_strings, const std::vector<std::string> & failing_strings) {
    fprintf(stderr, "⚫ Testing %s\n%s\n", test_desc.c_str(), grammar_str.c_str());
    fflush(stderr);

    auto grammar = build_grammar(grammar_str);

    // Save the original grammar stacks so that we can reset after every new string we want to test
    auto original_stacks = grammar->stacks;

    fprintf(stderr, "  🔵 Valid strings:\n");

    // Passing strings
    for (const auto & test_string : passing_strings) {
        fprintf(stderr, "    \"%s\" ", test_string.c_str());
        fflush(stderr);

        bool matched = match_string(test_string, grammar);

        if (!matched) {
            fprintf(stderr, "❌ (failed to match)\n");

            // DEBUG: Write strings to files so that we can analyze more easily with gbnf-validator program to see exactly where things failed.
            // DEBUG: Write the grammar_str to test-grammar-integration.grammar.gbnf
            FILE* grammar_file = fopen("test-grammar-integration.grammar.gbnf", "w");
            if (grammar_file) {
                fprintf(grammar_file, "%s", grammar_str.c_str());
                fclose(grammar_file);
            }

            // DEBUG: Write the test string to test-grammar-integration.string.txt
            FILE* string_file = fopen("test-grammar-integration.string.txt", "w");
            if (string_file) {
                fprintf(string_file, "%s", test_string.c_str());
                fclose(string_file);
            }

            fprintf(stderr, "\n NOTE: Debug grammar file generated. To analyze this failure in detail, run the following command:     ./llama-gbnf-validator test-grammar-integration.grammar.gbnf test-grammar-integration.string.txt\n\n");
        } else {
            fprintf(stdout, "✅︎\n");
        }

        assert(matched);

        // Reset the grammar stacks
        grammar->stacks = original_stacks;
    }

    fprintf(stderr, "  🟠 Invalid strings:\n");

    // Failing strings
    for (const auto & test_string : failing_strings) {
        fprintf(stderr, "    \"%s\" ", test_string.c_str());
        fflush(stderr);

        bool matched = match_string(test_string, grammar);

        if (matched) {
            fprintf(stderr, "❌ (incorrectly matched)\n");
        } else {
            fprintf(stdout, "✅︎\n");
        }
        assert(!matched);

        // Reset the grammar stacks
        grammar->stacks = original_stacks;
    }

    // Clean up allocated memory
    llama_grammar_free(grammar);
}
static void test_grammar(const std::string & test_desc, const std::string & grammar_str, const std::vector<std::string> & passing_strings, const std::vector<std::string> & failing_strings) {
    test(test_desc + ". Grammar: " + grammar_str, grammar_str, passing_strings, failing_strings);
}
static void test_schema(const std::string & test_desc, const std::string & schema_str, const std::vector<std::string> & passing_strings, const std::vector<std::string> & failing_strings) {
    test(test_desc + ". Schema: " + schema_str, json_schema_to_grammar(json::parse(schema_str)), passing_strings, failing_strings);
}

static void test_simple_grammar() {
    // Test case for a simple grammar
    test_grammar(
        "simple grammar",
        R"""(
            root ::= expr
            expr ::= term ("+" term)*
            term ::= number
            number ::= [0-9]+)""",
        // Passing strings
        {
            "42",
            "1+2+3+4+5",
            "123+456",
        },
        // Failing strings
        {
            "+",
            "/ 3",
            "1+2+3+4+5+",
            "12a45",
        }
    );
}

static void test_complex_grammar() {
    // Test case for a more complex grammar, with both failure strings and success strings
    test_grammar(
        "medium complexity grammar",
        // Grammar
        R"""(
            root ::= expression
            expression ::= term ws (("+"|"-") ws term)*
            term ::= factor ws (("*"|"/") ws factor)*
            factor ::= number | variable | "(" expression ")" | function-call
            number ::= [0-9]+
            variable ::= [a-zA-Z_][a-zA-Z0-9_]*
            function-call ::= variable ws "(" (expression ("," ws expression)*)? ")"
            ws ::= [ \t\n\r]?)""",
        // Passing strings
        {
            "42",
            "1*2*3*4*5",
            "x",
            "x+10",
            "x1+y2",
            "(a+b)*(c-d)",
            "func()",
            "func(x,y+2)",
            "a*(b+c)-d/e",
            "f(g(x),h(y,z))",
            "x + 10",
            "x1 + y2",
            "(a + b) * (c - d)",
            "func()",
            "func(x, y + 2)",
            "a * (b + c) - d / e",
            "f(g(x), h(y, z))",
            "123+456",
            "123*456*789-123/456+789*123",
            "123+456*789-123/456+789*123-456/789+123*456-789/123+456*789-123/456+789*123-456"
        },
        // Failing strings
        {
            "+",
            "/ 3x",
            "x + + y",
            "a * / b",
            "func(,)",
            "func(x y)",
            "(a + b",
            "x + y)",
            "a + b * (c - d",
            "42 +",
            "x +",
            "x + 10 +",
            "(a + b) * (c - d",
            "func(",
            "func(x, y + 2",
            "a * (b + c) - d /",
            "f(g(x), h(y, z)",
            "123+456*789-123/456+789*123-456/789+123*456-789/123+456*789-123/456+789*123-456/",
        }
    );
}

static void test_special_chars() {
    // A collection of tests to exercise special characters such as "."
    test_grammar(
        "special characters",
        // Grammar
        R"""(
            root ::= ... "abc" ...
            )""",
        // Passing strings
        {
            "abcabcabc",
            "aaaabcccc",
            // NOTE: Also ensures that multi-byte characters still count as a single character
            "🔵🟠✅abc❌🟠🔵"
        },
        // Failing strings
        {
            "aaabcccc",
            "aaaaabcccc",
            "aaaabccc",
            "aaaabccccc",
            "🔵🟠✅❌abc❌✅🟠🔵"
            "🔵🟠abc🟠🔵"
        }
    );
}

static void test_quantifiers() {
    // A collection of tests to exercise * + and ? quantifiers

    test_grammar(
        "* quantifier",
        // Grammar
        R"""(root ::= "a"*)""",
        // Passing strings
        {
            "",
            "a",
            "aaaaa",
            "aaaaaaaaaaaaaaaaaa",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        },
        // Failing strings
        {
            "b",
            "ab",
            "aab",
            "ba",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"
        }
    );
    test_grammar(
        "+ quantifier",
        // Grammar
        R"""(root ::= "a"+)""",
        // Passing strings
        {
            "a",
            "aaaaa",
            "aaaaaaaaaaaaaaaaaa",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        },
        // Failing strings
        {
            "",
            "b",
            "ab",
            "aab",
            "ba",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"
        }
    );
    test_grammar(
        "? quantifier",
        // Grammar
        R"""(root ::= "a"?)""",
        // Passing strings
        {
            "",
            "a"
        },
        // Failing strings
        {
            "b",
            "ab",
            "aa",
            "ba",
        }
    );
    test_grammar(
        "mixed quantifiers",
        // Grammar
        R"""(
            root ::= cons+ vowel* cons? (vowel cons)*
            vowel ::= [aeiouy]
            cons ::= [bcdfghjklmnpqrstvwxyz]
            )""",
        // Passing strings
        {
            "yes",
            "no",
            "noyes",
            "crwth",
            "four",
            "bryyyy",
        },
        // Failing strings
        {
            "yess",
            "yesno",
            "forty",
            "catyyy",
        }
    );
    test_grammar(
        "simple exact repetition",
        // Grammar
        R"""(
            root ::= [ab]{4}
        )""",
        // Passing strings
        {
            "aaaa",
            "bbbb",
            "abab",
        },
        // Failing strings
        {
            "a",
            "b",
            "aaaaa",
        }
    );
    test_grammar(
        "simple min repetition",
        // Grammar
        R"""(
            root ::= [ab]{4,}
        )""",
        // Passing strings
        {
            "aaaa",
            "aaaaab",
            "bbbb",
            "ababab",
        },
        // Failing strings
        {
            "",
            "aba",
        }
    );
    test_grammar(
        "simple max repetition",
        // Grammar
        R"""(
            root ::= [ab]{0,4}
        )""",
        // Passing strings
        {
            "",
            "a",
            "aa",
            "aaa",
            "aaab",
        },
        // Failing strings
        {
            "aaaaa",
        }
    );
    test_grammar(
        "min / max repetition",
        // Grammar
        R"""(
            root ::= ("0x" [A-F0-9]{2} " "?){3,5}
        )""",
        // Passing strings
        {
            "0xFF 0x12 0xAB",
            "0xFF 0x12 0xAB 0x00 0x00",
        },
        // Failing strings
        {
            "",
            "0xFF",
            "0xFF 0x12",
            "0xFF 0x12 0xAB 0x00 0x00 0x00",
        }
    );
}

static void test_failure_missing_root() {
    fprintf(stderr, "⚫ Testing missing root node:\n");
    // Test case for a grammar that is missing a root rule
    const std::string grammar_str = R"""(
        rot ::= expr
        expr ::= term ("+" term)*
        term ::= number
        number ::= [0-9]+)""";

    grammar_parser::parse_state parsed_grammar = grammar_parser::parse(grammar_str.c_str());

    // Ensure we parsed correctly
    assert(!parsed_grammar.rules.empty());

    // Ensure we do NOT have a root node
    assert(parsed_grammar.symbol_ids.find("root") == parsed_grammar.symbol_ids.end());
    fprintf(stderr, "  ✅︎ Passed\n");
}

static void test_failure_missing_reference() {
    fprintf(stderr, "⚫ Testing missing reference node:\n");

    // Test case for a grammar that is missing a referenced rule
    const std::string grammar_str =
        R"""(root ::= expr
        expr ::= term ("+" term)*
        term ::= numero
        number ::= [0-9]+)""";

    fprintf(stderr, "    Expected error:  ");

    grammar_parser::parse_state parsed_grammar = grammar_parser::parse(grammar_str.c_str());

    // Ensure we did NOT parsed correctly
    assert(parsed_grammar.rules.empty());

    fprintf(stderr, "    End of expected error.\n");
    fprintf(stderr, "  ✅︎ Passed\n");
}

static void test_failure_left_recursion() {
    fprintf(stderr, "⚫ Testing left recursion detection:\n");

    // Test simple left recursion detection
    const std::string simple_str = R"""(root ::= "a" | root "a")""";
    assert(test_build_grammar_fails(simple_str));

    // Test more complicated left recursion detection
    const std::string medium_str = R"""(
        root ::= asdf
        asdf ::= "a" | asdf "a"
        )""";
    assert(test_build_grammar_fails(medium_str));

    // Test even more complicated left recursion detection
    const std::string hard_str = R"""(
        root ::= asdf
        asdf ::= "a" | foo "b"
        foo ::= "c" | asdf "d" | "e")""";
    assert(test_build_grammar_fails(hard_str));

    // Test yet even more complicated left recursion detection
    const std::string hardest_str = R"""(
        root ::= asdf
        asdf ::= "a" | foo "b"
        foo ::= "c" | empty asdf "d" | "e"
        empty ::= "blah" | )""";
    assert(test_build_grammar_fails(hardest_str));

    fprintf(stderr, "  ✅︎ Passed\n");
}

static void test_json_schema() {
    // Note that this is similar to the regular grammar tests,
    //  but we convert each json schema to a grammar before parsing.
    // Otherwise, this test structure is the same.

    test_schema(
        "empty schema (object)",
        // Schema
        R"""(
            {}
        )""",
        // Passing strings
        {
            "{}",
            R"""({"foo": "bar"})""",
        },
        // Failing strings
        {
            "",
            "[]",
            "null",
            "\"\"",
            "true",
        }
    );

    test_schema(
        "exotic formats (list)",
        // Schema
        R"""(
            {
            "items": [
                { "format": "date" },
                { "format": "uuid" },
                { "format": "time" },
                { "format": "date-time" }
            ]
            }
        )""",
        // Passing strings
        {
            // "{}", // NOTE: This string passes for this schema on https://www.jsonschemavalidator.net/ -- should it?
            // "[]", // NOTE: This string passes for this schema on https://www.jsonschemavalidator.net/ -- should it?
            R"""(["2012-04-23", "12345678-1234-1234-1234-1234567890ab", "18:25:43.511Z", "2012-04-23T18:25:43.511Z"])""",
            //R"""(["2012-04-23","12345678-1234-1234-1234-1234567890ab"])""", // NOTE: This string passes for this schema on https://www.jsonschemavalidator.net/ -- should it?
            //R"""({"foo": "bar"})""", // NOTE: This string passes for this schema on https://www.jsonschemavalidator.net/ -- should it?
        },
        // Failing strings
        {
            R"""(["foo", "bar"])""",
            R"""(["12345678-1234-1234-1234-1234567890ab"])""",
        }
    );

    test_schema(
        "string",
        // Schema
        R"""(
            {
                "type": "string"
            }
        )""",
        // Passing strings
        {
            "\"foo\"",
            "\"bar\"",
            "\"\"",
        },
        // Failing strings
        {
            "{}",
            "\"foo\": \"bar\"",
        }
    );

    test_schema(
        "string w/ min length 1",
        // Schema
        R"""(
            {
                "type": "string",
                "minLength": 1
            }
        )""",
        // Passing strings
        {
            "\"foo\"",
            "\"bar\"",
        },
        // Failing strings
        {
            "\"\"",
            "{}",
            "\"foo\": \"bar\"",
        }
    );

    test_schema(
        "string w/ min length 3",
        // Schema
        R"""(
            {
                "type": "string",
                "minLength": 3
            }
        )""",
        // Passing strings
        {
            "\"foo\"",
            "\"bar\"",
            "\"foobar\"",
        },
        // Failing strings
        {
            "\"\"",
            "\"f\"",
            "\"fo\"",
        }
    );

    test_schema(
        "string w/ max length",
        // Schema
        R"""(
            {
                "type": "string",
                "maxLength": 3
            }
        )""",
        // Passing strings
        {
            "\"foo\"",
            "\"bar\"",
            "\"\"",
            "\"f\"",
            "\"fo\"",
        },
        // Failing strings
        {
            "\"foobar\"",
        }
    );

    test_schema(
        "string w/ min & max length",
        // Schema
        R"""(
            {
                "type": "string",
                "minLength": 1,
                "maxLength": 4
            }
        )""",
        // Passing strings
        {
            "\"foo\"",
            "\"bar\"",
            "\"f\"",
            "\"barf\"",
        },
        // Failing strings
        {
            "\"\"",
            "\"barfo\"",
            "\"foobar\"",
        }
    );

    test_schema(
        "boolean",
        // Schema
        R"""(
            {
                "type": "boolean"
            }
        )""",
        // Passing strings
        {
            "true",
            "false",
        },
        // Failing strings
        {
            "\"\"",
            "\"true\"",
            "True",
            "FALSE",
        }
    );

    test_schema(
        "integer",
        // Schema
        R"""(
            {
                "type": "integer"
            }
        )""",
        // Passing strings
        {
            "0",
            "12345",
            "1234567890123456"
        },
        // Failing strings
        {
            "",
            "01",
            "007",
            "12345678901234567"
        }
    );

    test_schema(
        "string const",
        // Schema
        R"""(
            {
                "const": "foo"
            }
        )""",
        // Passing strings
        {
            "\"foo\"",
        },
        // Failing strings
        {
            "foo",
            "\"bar\"",
        }
    );

    test_schema(
        "non-string const",
        // Schema
        R"""(
            {
                "const": true
            }
        )""",
        // Passing strings
        {
            "true",
        },
        // Failing strings
        {
            "",
            "foo",
            "\"true\"",
        }
    );

    test_schema(
        "non-string const",
        // Schema
        R"""(
            {
                "enum": ["red", "amber", "green", null, 42, ["foo"]]
            }
        )""",
        // Passing strings
        {
            "\"red\"",
            "null",
            "42",
            "[\"foo\"]",
        },
        // Failing strings
        {
            "",
            "420",
            "true",
            "foo",
        }
    );


    test_schema(
        "min+max items",
        // Schema
        R"""(
            {
                "items": {
                    "type": ["number", "integer"]
                },
                "minItems": 3,
                "maxItems": 5
            }
        )""",
        // Passing strings
        {
            "[1, 2, 3]",
            "[1, 2, 3, 4]",
            "[1, 2, 3, 4, 5]",
        },
        // Failing strings
        {
            "[1, 2]",
            "[1, 2, 3, 4, 5, 6]",
            "1"
        }
    );

    // Properties (from: https://json-schema.org/understanding-json-schema/reference/object#properties)
    test_schema(
        "object properties",
        // Schema
        R"""(
            {
            "type": "object",
            "properties": {
                "number": { "type": "number" },
                "street_name": { "type": "string" },
                "street_type": { "enum": ["Street", "Avenue", "Boulevard"] }
            }
            }
        )""",
        // Passing strings
        {
            R"""({ "number": 1600, "street_name": "Pennsylvania", "street_type":"Avenue"})""",
            // "By default, leaving out properties is valid"
            R"""({ "street_name": "Pennsylvania" })""",
            R"""({ "number": 1600, "street_name": "Pennsylvania" })""",
            // "By extension, even an empty object is valid"
            R"""({})""",
            // "By default, providing additional properties is valid"
#ifdef INCLUDE_FAILING_TESTS
            // TODO: The following should pass, but currently FAILS. Additional properties should be permitted by default.
            R"""({ "number": 1600, "street_name": "Pennsylvania", "street_type":"Avenue", "direction":"NW"})""",
            // TODO: Spaces should be permitted around enum values, but currently they fail to pass.
            R"""({ "number": 1600, "street_name": "Pennsylvania", "street_type": "Avenue" })""",
#endif
        },
        // Failing strings
        {
            // Change datatype from number to string
            R"""({ "number": "1600", "street_name": "Pennsylvania", "street_type":"Avenue"})""",
            // Reorder properties
            R"""({ "street_name": "Pennsylvania", "number": 1600 })""",
            // Reorder properties
            R"""({ "number": "1600", "street_name": "Pennsylvania", "street_type":"Avenue"})""",
        }
    );


    // Properties (from: https://json-schema.org/understanding-json-schema/reference/object#properties)
    test_schema(
        "object properties, additionalProperties: true",
        // Schema
        R"""(
            {
            "type": "object",
            "properties": {
                "number": { "type": "number" },
                "street_name": { "type": "string" },
                "street_type": { "enum": ["Street", "Avenue", "Boulevard"] }
            },
            "additionalProperties": true
            }
        )""",
        // Passing strings
        {
            // "By extension, even an empty object is valid"
            R"""({})""",
#ifdef INCLUDE_FAILING_TESTS
            // TODO: Following line should pass and doesn't
            R"""({"number":1600,"street_name":"Pennsylvania","street_type":"Avenue"})""",
            // "By default, leaving out properties is valid"
            // TODO: Following line should pass and doesn't
            R"""({ "street_name": "Pennsylvania" })""",
            // TODO: Following line should pass and doesn't
            R"""({ "number": 1600, "street_name": "Pennsylvania" })""",
            // "By default, providing additional properties is valid"
            // TODO: The following should pass, but currently FAILS. Additional properties should be permitted by default.
            R"""({ "number": 1600, "street_name": "Pennsylvania", "street_type":"Avenue", "direction":"NW"})""",
            // TODO: Spaces should be permitted around enum values, but currently they fail to pass.
            R"""({ "number": 1600, "street_name": "Pennsylvania", "street_type": "Avenue" })""",
#endif
        },
        // Failing strings
        {
            // Change datatype from number to string
            R"""({ "number": "1600", "street_name": "Pennsylvania", "street_type":"Avenue"})""",
            // Reorder properties
            R"""({ "street_name": "Pennsylvania", "number": 1600, "street_type":"Avenue"})""",
        }
    );

    // Additional properties: false
    test_schema(
        "required + optional props each in original order",
        // Schema
        R"""(
            {
            "type": "object",
            "properties": {
                "number": { "type": "number" },
                "street_name": { "type": "string" },
                "street_type": { "enum": ["Street", "Avenue", "Boulevard"] }
            },
            "additionalProperties": false
            }
        )""",
        // Passing strings
        {
            R"""({ "street_name": "Pennsylvania" })""",
            R"""({ "number": 1600, "street_type":"Avenue"})""",
            R"""({ "number": 1600, "street_name": "Pennsylvania" })""",
            R"""({ "number": 1600, "street_name": "Pennsylvania", "street_type":"Avenue"})""",
#ifdef INCLUDE_FAILING_TESTS
            // TODO: Spaces should be permitted around enum values, but currently they fail to pass.
            R"""({ "number": 1600, "street_name": "Pennsylvania", "street_type": "Avenue" })""",
#endif
        },
        // Failing strings
        {
            // Reorder properties
            R"""({ "street_type": "Avenue", "number": 1600 })""",
            // Add "direction"
            R"""({ "number": 1600, "street_name": "Pennsylvania", "street_type": "Avenue", "direction": "NW" })""",
        }
    );

    test_schema(
        "required + optional props each in original order",
        // Schema
        R"""(
            {
                "properties": {
                    "b": {"type": "string"},
                    "a": {"type": "string"},
                    "d": {"type": "string"},
                    "c": {"type": "string"}
                },
                "required": ["a", "b"],
                "additionalProperties": false
            }
        )""",
        // Passing strings
        {
            R"""({"b": "foo", "a": "bar"})""",
            R"""({"b":"foo","a":"bar","d":"qux"})""",
            R"""({"b":"foo", "a":"bar", "d":"qux", "c":"baz"})""",
        },
        // Failing strings
        {
            R"""({"a": "foo", "b": "bar"})""",
            R"""({"b": "bar"})""",
            R"""({"a": "foo", "c": "baz"})""",
            R"""({"a":"foo", "b":"bar", "c":"baz", "d":"qux"})""",
        }
    );

    // NOTE: Example from https://json-schema.org/learn/getting-started-step-by-step#define-required-properties
    test_schema(
        "required props",
        // Schema
        R"""(
            {
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "$id": "https://example.com/product.schema.json",
            "title": "Product",
            "description": "A product from Acme's catalog",
            "type": "object",
            "properties": {
                "productId": {
                "description": "The unique identifier for a product",
                "type": "integer"
                },
                "productName": {
                "description": "Name of the product",
                "type": "string"
                },
                "price": {
                "description": "The price of the product",
                "type": "number",
                "exclusiveMinimum": 0
                },
                "tags": {
                "description": "Tags for the product",
                "type": "array",
                "items": {
                    "type": "string"
                },
                "minItems": 1,
                "uniqueItems": true
                },
                "dimensions": {
                "type": "object",
                "properties": {
                    "length": {
                    "type": "number"
                    },
                    "width": {
                    "type": "number"
                    },
                    "height": {
                    "type": "number"
                    }
                },
                "required": [ "length", "width", "height" ]
                }
            },
            "required": [ "productId", "productName", "price" ]
            }
        )""",
        // Passing strings
        {
            R"""({"productId": 1, "productName": "A green door", "price": 12.50})""",
            R"""({"productId": 1, "productName": "A green door", "price": 12.50, "tags": ["home", "green"]})""",
            R"""({"productId": 1, "productName": "A green door", "price": 12.50, "tags": ["home", "green"], "dimensions": {"length": 785, "width": 250.5, "height": -0.359}})""",
        },
        // Failing strings
        {
            R"""({})""", // Missing all required properties
            R"""({"productName": "A green door", "price": 12.50, "productId": 1})""", // Out of order properties
            // TODO: The following line should fail, but currently it passes. `exclusiveMinimum` is not supported, as it would likely be too difficult to implement.
            //  Perhaps special checks for minimum and maximum values of 0 could be added (since that's relatively easy to do with grammars), but anything else would likely be too complex.
            // R"""({"productId": 1, "productName": "A green door", "price": -12.50})""",
            R"""({"productId": 1, "productName": "A green door"})""", // Missing required property (price)
            R"""({"productName": "A green door", "price": 12.50})""", // Missing required property (productId)
            R"""({"productId": 1, "productName": "A green door", "price": 12.50, "tags": []})""", // tags is empty, but minItems is 1
            R"""({"productId": 1, "productName": "A green door", "price": 12.50, "dimensions": {"length": 785, "width": 250.5, "height": -0.359}, "tags": ["home", "green"]})""", // Tags and dimensions are out of order
            // TODO: The following line should fail, but currently it passes. `uniqueItems` is not supported, as it would likely be too difficult to implement.
            // R"""({"productId": 1, "productName": "A green door", "price": 12.50, "tags": ["home", "green", "home"]})""",
        }
    );
}

int main() {
    fprintf(stdout, "Running grammar integration tests...\n");
    test_simple_grammar();
    test_complex_grammar();
    test_special_chars();
    test_quantifiers();
    test_failure_missing_root();
    test_failure_missing_reference();
    test_failure_left_recursion();
    test_json_schema();
    fprintf(stdout, "All tests passed.\n");
    return 0;
}
