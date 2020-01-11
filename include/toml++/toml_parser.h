#pragma once
#include "toml_utf8.h"
#include "toml_table.h"
#include "toml_array.h"
#include "toml_value.h"

namespace toml
{
	#if TOML_EXCEPTIONS

	using parse_result = table;

	#else

	class parse_result final
	{
		private:
			bool is_err;
			std::aligned_storage_t<
				(sizeof(table) < sizeof(parse_error) ? sizeof(parse_error) : sizeof(table)),
				(alignof(table) < alignof(parse_error) ? alignof(parse_error) : alignof(table))
			> storage;

			void destroy() noexcept
			{
				if (is_err)
					std::launder(reinterpret_cast<parse_error*>(&storage))->~parse_error();
				else
					std::launder(reinterpret_cast<table*>(&storage))->~table();
			}

		public:

			[[nodiscard]] bool succeeded() const noexcept { return !is_err; }
			[[nodiscard]] bool failed() const noexcept { return is_err; }
			[[nodiscard]] explicit operator bool() const noexcept { return !is_err; }

			[[nodiscard]] table& get() & noexcept
			{
				TOML_ASSERT(!is_err);
				return *std::launder(reinterpret_cast<table*>(&storage));
			}
			[[nodiscard]] table&& get() && noexcept
			{
				TOML_ASSERT(!is_err);
				return std::move(*std::launder(reinterpret_cast<table*>(&storage)));
			}
			[[nodiscard]] const table& get() const& noexcept
			{
				TOML_ASSERT(!is_err);
				return *std::launder(reinterpret_cast<const table*>(&storage));
			}

			[[nodiscard]] parse_error& error() & noexcept
			{
				TOML_ASSERT(is_err);
				return *std::launder(reinterpret_cast<parse_error*>(&storage));
			}
			[[nodiscard]] parse_error&& error() && noexcept
			{
				TOML_ASSERT(is_err);
				return std::move(*std::launder(reinterpret_cast<parse_error*>(&storage)));
			}
			[[nodiscard]] const parse_error& error() const& noexcept
			{
				TOML_ASSERT(is_err);
				return *std::launder(reinterpret_cast<const parse_error*>(&storage));
			}

			[[nodiscard]] table& operator* () & noexcept { return get(); }
			[[nodiscard]] table&& operator* () && noexcept { return std::move(get()); }
			[[nodiscard]] const table& operator* () const & noexcept { return get(); }

			[[nodiscard]] table* operator-> () noexcept { return &get(); }
			[[nodiscard]] const table* operator-> () const noexcept { return &get(); }

			[[nodiscard]] operator table& () noexcept { return get(); }
			[[nodiscard]] operator table&& () noexcept { return std::move(get()); }
			[[nodiscard]] operator const table& () const noexcept { return get(); }

			[[nodiscard]] explicit operator parse_error& () noexcept { return error(); }
			[[nodiscard]] explicit operator parse_error && () noexcept { return std::move(error()); }
			[[nodiscard]] explicit operator const parse_error& () const noexcept { return error(); }

			TOML_NODISCARD_CTOR
			explicit parse_result(table&& tbl) noexcept
				: is_err{ false }
			{
				::new (&storage) table{ std::move(tbl) };
			}

			TOML_NODISCARD_CTOR
			explicit parse_result(parse_error&& err) noexcept
				: is_err{ true }
			{
				::new (&storage) parse_error{ std::move(err) };
			}

			TOML_NODISCARD_CTOR
			parse_result(parse_result&& res) noexcept
				: is_err{ res.is_err }
			{
				if (is_err)
					::new (&storage) parse_error{ std::move(res).error() };
				else
					::new (&storage) table{ std::move(res).get() };
			}

			parse_result& operator=(parse_result&& rhs) noexcept
			{
				if (is_err != rhs.is_err)
				{
					destroy();
					is_err = rhs.is_err;
					if (is_err)
						::new (&storage) parse_error{ std::move(rhs).error() };
					else
						::new (&storage) table{ std::move(rhs).get() };
				}
				else
				{
					if (is_err)
						error() = std::move(rhs).error();
					else
						get() = std::move(rhs).get();
				}
				return *this;
			}

			~parse_result() noexcept
			{
				destroy();
			}
	};

	#endif
}

namespace toml::impl
{
	#if TOML_EXCEPTIONS
		#define TOML_ERROR_CHECK(...)	(void)0
		#define TOML_ERROR				throw toml::parse_error
	#else
		#define TOML_ERROR_CHECK(...)	if (err) return __VA_ARGS__
		#define TOML_ERROR				err.emplace
	#endif

	#if !TOML_EXCEPTIONS || defined(__INTELLISENSE__)
		#define TOML_NORETURN
	#else
		#define TOML_NORETURN			[[noreturn]]
	#endif

	template <int> struct parse_integer_traits;
	template <> struct parse_integer_traits<2> final
	{
		static constexpr auto qualifier = "binary"sv;
		static constexpr auto is_digit = is_binary_digit;
		static constexpr auto is_signed = false;
		static constexpr auto buffer_length = 63;
		static constexpr char32_t prefix_codepoint = U'b';
		static constexpr char prefix = 'b';
	};
	template <> struct parse_integer_traits<8> final
	{
		static constexpr auto qualifier = "octal"sv;
		static constexpr auto is_digit = is_octal_digit;
		static constexpr auto is_signed = false;
		static constexpr auto buffer_length = 21; // strlen("777777777777777777777")
		static constexpr char32_t prefix_codepoint = U'o';
		static constexpr char prefix = 'o';
	};
	template <> struct parse_integer_traits<10> final
	{
		static constexpr auto qualifier = "decimal"sv;
		static constexpr auto is_digit = is_decimal_digit;
		static constexpr auto is_signed = true;
		static constexpr auto buffer_length = 19; //strlen("9223372036854775807")
	};
	template <> struct parse_integer_traits<16> final
	{
		static constexpr auto qualifier = "hexadecimal"sv;
		static constexpr auto is_digit = is_hexadecimal_digit;
		static constexpr auto is_signed = false;
		static constexpr auto buffer_length = 16; //strlen("7FFFFFFFFFFFFFFF")
		static constexpr char32_t prefix_codepoint = U'x';
		static constexpr char prefix = 'x';
	};

	class parser final
	{
		private:
			utf8_buffered_reader reader;
			table root;
			source_position prev_pos = { 1, 1 };
			const utf8_codepoint* cp = {};
			std::vector<table*> implicit_tables;
			std::vector<table*> dotted_key_tables;
			std::vector<array*> table_arrays;
			std::string recording_buffer; //for diagnostics 
			bool recording = false;
			#if !TOML_EXCEPTIONS
			mutable std::optional<toml::parse_error> err;
			#endif

			[[nodiscard]]
			source_position current_position_or_assumed_next() const noexcept
			{
				if (cp)
					return cp->position;
				return { prev_pos.line, static_cast<source_index>(prev_pos.column + 1u) };
			}

			template <typename... T>
			TOML_NORETURN
			void abort_with_error(T &&... args) const TOML_MAY_THROW
			{
				TOML_ERROR_CHECK();

				if constexpr (sizeof...(T) == 0_sz)
					TOML_ERROR( "An unspecified error occurred", current_position_or_assumed_next(), reader.source_path() );
				else
				{
					static constexpr auto buf_size = 512_sz;
					TOML_GCC_ATTR(uninitialized) char buf[buf_size];
					auto ptr = buf;
					const auto concatenator = [&](auto&& arg) noexcept //a.k.a. "no stringstreams, thanks"
					{
						using arg_t = remove_cvref_t<decltype(arg)>;
						if constexpr (std::is_same_v<arg_t, std::string_view> || std::is_same_v<arg_t, std::string>)
						{
							std::memcpy(ptr, arg.data(), arg.length());
							ptr += arg.length();
						}
						else if constexpr (std::is_same_v<arg_t, utf8_codepoint>)
						{
							toml::string_view cp_view;
							if (arg.value <= U'\x1F') TOML_UNLIKELY
								cp_view = low_character_escape_table[arg.value];
							else if (arg.value == U'\x7F')  TOML_UNLIKELY
								cp_view = TOML_STRING_PREFIX("\\u007F"sv);
							else
								cp_view = arg.template as_view<string_char>();

							std::memcpy(ptr, cp_view.data(), cp_view.length());
							ptr += cp_view.length();
						}
						else if constexpr (std::is_same_v<arg_t, char>)
						{
							*ptr++ = arg;
						}
						else if constexpr (std::is_same_v<arg_t, bool>)
						{
							const auto boolval = arg ? "true"sv : "false"sv;
							std::memcpy(ptr, boolval.data(), boolval.length());
							ptr += boolval.length();
						}
						else if constexpr (std::is_same_v<arg_t, node_type>)
						{
							const auto str = impl::node_type_friendly_names[
								static_cast<std::underlying_type_t<node_type>>(arg)
							];
							std::memcpy(ptr, str.data(), str.length());
							ptr += str.length();
						}
						else if constexpr (std::is_floating_point_v<arg_t>)
						{
							#if TOML_USE_STREAMS_FOR_FLOATS
							{
								std::ostringstream oss;
								oss << arg;
								const auto str = oss.str();
								std::memcpy(ptr, str.c_str(), str.length());
								ptr += str.length();
							}
							#else
							{
								const auto result = std::to_chars(ptr, buf + buf_size, arg);
								ptr = result.ptr;
							}
							#endif
						}
						else if constexpr (std::is_integral_v<arg_t>)
						{
							const auto result = std::to_chars(ptr, buf + buf_size, arg);
							ptr = result.ptr;
						}
					};
					(concatenator(std::forward<T>(args)), ...);
					*ptr = '\0';
					#if TOML_EXCEPTIONS
						TOML_ERROR( buf, current_position_or_assumed_next(), reader.source_path() );
					#else
						TOML_ERROR( std::string(buf, ptr - buf), current_position_or_assumed_next(), reader.source_path());
					#endif
				}
			}

			void go_back(size_t count = 1_sz) noexcept
			{
				TOML_ERROR_CHECK();
				TOML_ASSERT(count);

				cp = reader.step_back(count);
				prev_pos = cp->position;
			}

			void advance() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK();
				TOML_ASSERT(cp);

				prev_pos = cp->position;
				cp = reader.read_next();

				#if !TOML_EXCEPTIONS
				if (reader.error())
				{
					err = std::move(reader.error());
					return;
				}
				#endif

				if (recording && cp)
					recording_buffer.append(cp->as_view<char>());
			}

			void start_recording(bool include_current = true) noexcept
			{
				TOML_ERROR_CHECK();

				recording = true;
				recording_buffer.clear();
				if (include_current && cp)
					recording_buffer.append(cp->as_view<char>());
			}

			void stop_recording(size_t pop_bytes = 0_sz) noexcept
			{
				TOML_ERROR_CHECK();

				recording = false;
				if (pop_bytes)
				{
					if (pop_bytes >= recording_buffer.length())
						recording_buffer.clear();
					else
						recording_buffer.erase(
							recording_buffer.cbegin() + static_cast<ptrdiff_t>(recording_buffer.length() - pop_bytes),
							recording_buffer.cend()
						);
				}
			}

			bool consume_leading_whitespace() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});

				bool consumed = false;
				while (cp && is_whitespace(*cp))
				{
					consumed = true;
					advance();
					TOML_ERROR_CHECK({});
				}
				return consumed;
			}

			bool consume_line_break() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});

				if (!cp || !is_line_break(*cp))
					return false;

				if (*cp == U'\r')
				{
					advance();  // skip \r
					TOML_ERROR_CHECK({});

					if (!cp)
						abort_with_error("Encountered EOF while consuming CRLF"sv);
					if (*cp != U'\n')
						abort_with_error("Encountered unexpected character while consuming CRLF"sv);
				}
				advance(); // skip \n (or other single-character line ending)
				TOML_ERROR_CHECK({});

				return true;
			}

			bool consume_rest_of_line() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});

				if (!cp)
					return false;

				do
				{
					if (is_line_break(*cp))
						return consume_line_break();
					else
						advance();

					TOML_ERROR_CHECK({});
				}
				while (cp);
				return true;
			}

			bool consume_comment() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});

				if (!cp || *cp != U'#')
					return false;

				advance(); //skip the '#'
				TOML_ERROR_CHECK({});

				while (cp)
				{
					if (consume_line_break())
						return true;

					if constexpr (TOML_LANG_HIGHER_THAN(0, 5, 0)) // toml/issues/567
					{
						if (*cp <= U'\u0008'
							|| (*cp >= U'\u000A' && *cp <= U'\u001F')
							|| *cp == U'\u007F')
							abort_with_error(
								"Encountered unexpected character while parsing comment; control characters "
								"U+0000-U+0008, U+000A-U+001F and U+007F are explicitly prohibited from appearing "
								"in comments."sv
							);
					}

					advance();
					TOML_ERROR_CHECK({});
				}

				return true;
			}

			[[nodiscard]]
			bool consume_expected_sequence(std::u32string_view seq) TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});

				for (auto c : seq)
				{
					if (!cp || *cp != c)
						return false;

					advance();
					TOML_ERROR_CHECK({});
				}
				return true;
			}

			template <typename T, size_t N>
			[[nodiscard]]
			bool consume_digit_sequence(T(&digits)[N]) TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});

				for (size_t i = 0; i < N; i++)
				{
					if (!cp || !is_decimal_digit(*cp))
						return false;
					digits[i] = static_cast<T>(*cp - U'0');
					advance();
					TOML_ERROR_CHECK({});
				}
				return true;
			}

			template <typename T, size_t N>
			[[nodiscard]]
			size_t consume_variable_length_digit_sequence(T(&buffer)[N]) TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});

				size_t i = {};
				for (; i < N; i++)
				{
					if (!cp || !is_decimal_digit(*cp))
						break;
					buffer[i] = static_cast<T>(*cp - U'0');
					advance();
					TOML_ERROR_CHECK({});
				}
				return i;
			}

			template <bool MULTI_LINE>
			[[nodiscard]]
			string parse_basic_string() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && *cp == U'"');

				const auto eof_check = [this]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error(
							"Encountered EOF while parsing "sv,
							(MULTI_LINE ? "multi-line "sv : ""sv),
							node_type::string
						);
				};

				// skip the '"'
				advance();
				eof_check();
				TOML_ERROR_CHECK({});

				string str;
				bool escaped = false, skipping_whitespace = false;
				while (cp)
				{
					if (escaped)
					{
						escaped = false;

						// handle 'line ending slashes' in multi-line mode
						if constexpr (MULTI_LINE)
						{
							if (is_line_break(*cp))
							{
								consume_line_break();
								skipping_whitespace = true;
								TOML_ERROR_CHECK({});
								continue;
							}
						}

						// skip the escaped character
						const auto escaped_codepoint = cp->value;
						advance();
						TOML_ERROR_CHECK({});

						switch (escaped_codepoint)
						{
							// 'regular' escape codes
							case U'b': str += TOML_STRING_PREFIX('\b'); break;
							case U'f': str += TOML_STRING_PREFIX('\f'); break;
							case U'n': str += TOML_STRING_PREFIX('\n'); break;
							case U'r': str += TOML_STRING_PREFIX('\r'); break;

							case U's':
							{
								if constexpr (!TOML_LANG_HIGHER_THAN(0, 5, 0)) // toml/issues/622
								{
									abort_with_error("Escape sequence '\\s' (Space, U+0020) is not supported "
										"in TOML 0.5.0 and earlier."sv
									);
									break;
								}
								else
								{
									str += TOML_STRING_PREFIX(' ');
									break;
								}
							}
							 
							case U't': str += TOML_STRING_PREFIX('\t'); break;
							case U'"': str += TOML_STRING_PREFIX('"'); break;
							case U'\\': str += TOML_STRING_PREFIX('\\'); break;

							// unicode scalar sequences
							case U'u': [[fallthrough]];
							case U'U':
							{
								uint32_t place_value = escaped_codepoint == U'U' ? 0x10000000u : 0x1000u;
								uint32_t sequence_value{};
								while (place_value)
								{
									eof_check();

									if (!is_hexadecimal_digit(*cp))
										abort_with_error(
											"Encountered unexpected character while parsing "sv,
											(MULTI_LINE ? "multi-line "sv : ""sv),
											node_type::string,
											"; expected hex digit, saw '\\"sv, *cp, "'"sv
										);
									sequence_value += place_value * (
										*cp >= U'A'
											? 10u + static_cast<uint32_t>(*cp - (*cp >= U'a' ? U'a' : U'A'))
											: static_cast<uint32_t>(*cp - U'0')
										);
									place_value /= 16u;
									advance();
									TOML_ERROR_CHECK({});
								}

								if ((sequence_value > 0xD7FFu && sequence_value < 0xE000u) || sequence_value > 0x10FFFFu)
									abort_with_error(
										"Encountered unknown unicode scalar sequence while parsing "sv,
										(MULTI_LINE ? "multi-line "sv : ""sv),
										node_type::string
									);

								if (sequence_value <= 0x7Fu) //ascii
									str += static_cast<string_char>(sequence_value & 0x7Fu);
								else if (sequence_value <= 0x7FFu)
								{
									str += static_cast<string_char>(0xC0u | ((sequence_value >> 6) & 0x1Fu));
									str += static_cast<string_char>(0x80u | (sequence_value & 0x3Fu));
								}
								else if (sequence_value <= 0xFFFFu)
								{
									str += static_cast<string_char>(0xE0u | ((sequence_value >> 12) & 0x0Fu));
									str += static_cast<string_char>(0x80u | ((sequence_value >> 6) & 0x1Fu));
									str += static_cast<string_char>(0x80u | (sequence_value & 0x3Fu));
								}
								else
								{
									str += static_cast<string_char>(0xF0u | ((sequence_value >> 18) & 0x07u));
									str += static_cast<string_char>(0x80u | ((sequence_value >> 12) & 0x3Fu));
									str += static_cast<string_char>(0x80u | ((sequence_value >> 6) & 0x3Fu));
									str += static_cast<string_char>(0x80u | (sequence_value & 0x3Fu));
								}

								break;
							}

							// ???
							default:
								abort_with_error(
									"Encountered unexpected character while parsing "sv,
									(MULTI_LINE ? "multi-line "sv : ""sv),
									node_type::string,
									"; unknown escape sequence '\\"sv, *cp, "'"sv
								);
						}
					}
					else TOML_LIKELY
					{
						// handle closing delimiters
						if (*cp == U'"')
						{
							if constexpr (MULTI_LINE)
							{
								advance();
								eof_check();
								TOML_ERROR_CHECK({});
								const auto second = cp->value;

								advance();
								eof_check();
								TOML_ERROR_CHECK({});
								const auto third = cp->value;

								if (second == U'"' && third == U'"')
								{
									advance(); // skip the third closing delimiter
									TOML_ERROR_CHECK({});

									//multi-line basic strings are allowed one additional terminating '"'
									//so that things like this work: """this is a "quote""""
									if (cp && *cp == U'"')
									{
										str += TOML_STRING_PREFIX('"');
										advance(); // skip the final closing delimiter
										TOML_ERROR_CHECK({});
									}

									return str;
								}
								else
								{
									str += TOML_STRING_PREFIX('"');
									go_back(1_sz);
									skipping_whitespace = false;
									continue;
								}
							}
							else
							{
								advance(); // skip the closing delimiter
								TOML_ERROR_CHECK({});
								return str;
							}
						}

						// handle escapes
						if (*cp == U'\\')
						{
							advance(); // skip the '\'
							TOML_ERROR_CHECK({});
							skipping_whitespace = false;
							escaped = true;
							continue;
						}

						// handle line endings in multi-line mode
						if constexpr (MULTI_LINE)
						{
							if (is_line_break(*cp))
							{
								consume_line_break();
								TOML_ERROR_CHECK({});
								if (!str.empty() && !skipping_whitespace)
									str += TOML_STRING_PREFIX('\n');
								continue;
							}
						}

						// handle illegal characters
						if (*cp <= U'\u0008'
							|| (*cp >= U'\u000A' && *cp <= U'\u001F')
							|| *cp == U'\u007F')
							abort_with_error(
								"Encountered unexpected character while parsing "sv,
								(MULTI_LINE ? "multi-line "sv : ""sv),
								node_type::string,
								"; control characters must be escaped with back-slashes."sv
							);

						if constexpr (MULTI_LINE)
						{
							if (!skipping_whitespace || !is_whitespace(*cp))
							{
								skipping_whitespace = false;
								str.append(cp->as_view());
							}
						}
						else
							str.append(cp->as_view());

						advance();
						TOML_ERROR_CHECK({});
					}
				}

				abort_with_error(
					"Encountered EOF while parsing "sv,
					(MULTI_LINE ? "multi-line "sv : ""sv),
					node_type::string
				);
				TOML_ERROR_CHECK({});
				TOML_UNREACHABLE;
			}

			template <bool MULTI_LINE>
			[[nodiscard]]
			string parse_literal_string() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && *cp == U'\'');

				const auto eof_check = [this]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error(
							"Encountered EOF while parsing "sv,
							(MULTI_LINE ? "multi-line "sv : ""sv),
							"literal "sv, node_type::string
						);
				};

				// skip the delimiter
				advance();
				eof_check();

				string str;
				while (cp)
				{
					TOML_ERROR_CHECK({});

					// handle closing delimiters
					if (*cp == U'\'')
					{
						if constexpr (MULTI_LINE)
						{
							advance();
							eof_check();
							TOML_ERROR_CHECK({});
							const auto second = cp->value;

							advance();
							eof_check();
							TOML_ERROR_CHECK({});
							const auto third = cp->value;

							if (second == U'\'' && third == U'\'')
							{
								advance(); // skip the third closing delimiter
								TOML_ERROR_CHECK({});
								return str;
							}
							else
							{
								str += TOML_STRING_PREFIX('\'');
								go_back(1_sz);
								continue;
							}
						}
						else
						{
							advance(); // skip the closing delimiter
							TOML_ERROR_CHECK({});
							return str;
						}
					}

					// handle line endings in multi-line mode
					if constexpr (MULTI_LINE)
					{
						if (is_line_break(*cp))
						{
							consume_line_break();
							if (!str.empty())
								str += TOML_STRING_PREFIX('\n');
							continue;
						}
					}

					// handle illegal characters
					if (*cp <= U'\u0008'
						|| (*cp >= U'\u000A' && *cp <= U'\u001F')
						|| *cp == U'\u007F')
						abort_with_error(
							"Encountered unexpected character while parsing "sv,
							(MULTI_LINE ? "multi-line "sv : ""sv),
							"literal "sv, node_type::string,
							"; control characters may not appear in literal strings"sv
						);

					str.append(cp->as_view());
					advance();
					TOML_ERROR_CHECK({});
				}

				abort_with_error("Encountered EOF while parsing "sv,
					(MULTI_LINE ? "multi-line "sv : ""sv),
					"literal "sv, node_type::string
				);
				TOML_ERROR_CHECK({});
				TOML_UNREACHABLE;
			}

			[[nodiscard]]
			string parse_string() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && is_string_delimiter(*cp));

				// get the first three characters to determine the string type
				const auto first = cp->value;
				advance();
				TOML_ERROR_CHECK({});
				if (!cp)
					abort_with_error("Encountered EOF while parsing "sv, node_type::string);
				const auto second = cp->value;
				advance();
				TOML_ERROR_CHECK({});
				const auto third = cp ? cp->value : U'\0';

				// if we were eof at the third character then first and second need to be
				// the same string character (otherwise it's an unterminated string)
				if (!cp)
				{
					if (second == first)
						return string{};
					abort_with_error("Encountered EOF while parsing "sv, node_type::string);
				}
					
				// if the first three characters are all the same string delimiter then
				// it's a multi-line string.
				if (first == second && first == third)
				{
					return first == U'\''
						? parse_literal_string<true>()
						: parse_basic_string<true>();
				}

				// otherwise it's just a regular string.
				else
				{
					// step back two characters so that the current
					// character is the string delimiter
					go_back(2_sz);

					return first == U'\''
						? parse_literal_string<false>()
						: parse_basic_string<false>();
				}
			}

			[[nodiscard]]
			string parse_bare_key_segment() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && is_bare_key_start_character(*cp));

				string segment;

				while (cp)
				{
					if (!is_bare_key_character(*cp))
						break;

					segment.append(cp->as_view());
					advance();
					TOML_ERROR_CHECK({});
				}

				return segment;
			}

			[[nodiscard]]
			bool parse_bool() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && (*cp == U't' || *cp == U'f'));

				auto result = *cp == U't';
				if (!consume_expected_sequence(result ? U"true"sv : U"false"sv))
				{
					if (!cp)
						abort_with_error("Encountered EOF while parsing "sv, node_type::boolean);
					else
						abort_with_error(
							"Encountered unexpected character while parsing "sv, node_type::boolean,
							"; expected 'true' or 'false', saw '"sv, *cp, '\''
						);
				}
				TOML_ERROR_CHECK({});

				if (cp && !is_value_terminator(*cp))
					abort_with_error(
						"Encountered unexpected character while parsing "sv, node_type::boolean,
						"; expected value-terminator, saw '"sv,
						*cp, '\''
					);

				TOML_ERROR_CHECK({});
				return result;
			}

			[[nodiscard]]
			double parse_inf_or_nan() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && (*cp == U'i' || *cp == U'n' || *cp == U'+' || *cp == U'-'));

				const auto eof_check = [this]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error("Encountered EOF while parsing "sv, node_type::floating_point);
				};

				const int sign = *cp == U'-' ? -1 : 1;
				if (*cp == U'+' || *cp == U'-')
				{
					advance();
					eof_check();
					TOML_ERROR_CHECK({});
				}

				const bool inf = *cp == U'i';
				if (!consume_expected_sequence(inf ? U"inf"sv : U"nan"sv))
				{
					eof_check();
					abort_with_error(
						"Encountered unexpected character while parsing "sv, node_type::floating_point,
						"; expected '"sv, inf ? "inf"sv : "nan"sv, "', saw '"sv, *cp, '\''
					);
				}
				TOML_ERROR_CHECK({});

				if (cp && !is_value_terminator(*cp))
					abort_with_error(
						"Encountered unexpected character while parsing "sv, node_type::floating_point,
						"; expected value-terminator, saw '"sv, *cp, '\''
					);
				TOML_ERROR_CHECK({});

				return inf
					? sign * std::numeric_limits<double>::infinity()
					: std::numeric_limits<double>::quiet_NaN();
			}

			TOML_PUSH_WARNINGS
			TOML_DISABLE_SWITCH_WARNINGS
			TOML_DISABLE_INIT_WARNINGS

			[[nodiscard]]
			double parse_float() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && (*cp == U'+' || *cp == U'-' || is_decimal_digit(*cp)));

				const auto eof_check = [this]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error("Encountered EOF while parsing "sv, node_type::floating_point);
				};

				// sign
				const int sign = *cp == U'-' ? -1 : 1;
				if (*cp == U'+' || *cp == U'-')
				{
					advance();
					eof_check();
					TOML_ERROR_CHECK({});
				}

				// consume value chars
				TOML_GCC_ATTR(uninitialized) char chars[64];
				size_t length = {};
				const utf8_codepoint* prev = {};
				bool seen_decimal = false, seen_exponent_sign = false, seen_exponent = false;
				while (true)
				{
					if (!cp || is_value_terminator(*cp))
						break;

					if (*cp == U'_')
					{
						if (!prev || !is_decimal_digit(*prev))
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::floating_point,
								"; underscores may only follow digits"sv
							);
						prev = cp;
						advance();
						TOML_ERROR_CHECK({});
						continue;
					}

					if (*cp == U'.')
					{
						if (seen_decimal)
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::floating_point,
								"; decimal points may appear only once"sv
							);
						if (seen_exponent)
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::floating_point,
								"; decimal points may not appear after exponents"sv
							);
						seen_decimal = true;
					}
					else if (*cp == U'e' || *cp == U'E')
					{
						if (seen_exponent)
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::floating_point,
								"; exponents may appear only once"sv
							);
						seen_exponent = true;
					}
					else if (*cp == U'+' || *cp == U'-')
					{
						if (!seen_exponent || !(*prev == U'e' || *prev == U'E'))
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::floating_point,
								"; exponent signs must immediately follow 'e'"sv
							);
						if (seen_exponent_sign)
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::floating_point,
								"; exponents signs may appear only once"sv
							);
						seen_exponent_sign = true;
					}
					else if (!is_decimal_digit(*cp))
						abort_with_error("Encountered unexpected character while parsing "sv,
							node_type::floating_point, "; expected decimal digit, saw '"sv, *cp, '\''
						);

					if (length == sizeof(chars))
						abort_with_error(
							"Error parsing "sv, node_type::floating_point,
							"; exceeds maximum length of "sv, sizeof(chars), " characters"sv
						);

					chars[length++] = static_cast<char>(cp->bytes[0]);
					prev = cp;
					advance();

					TOML_ERROR_CHECK({});
				}
				if (prev && *prev == U'_')
				{
					eof_check();
					abort_with_error(
						"Encountered unexpected character while parsing "sv, node_type::floating_point,
						"; expected decimal digit, saw '"sv, *cp, '\''
					);
				}

				TOML_ERROR_CHECK({});

				// convert to double
				TOML_GCC_ATTR(uninitialized) double result;
				#if TOML_USE_STREAMS_FOR_FLOATS
				{
					std::stringstream ss;
					ss.write(chars, static_cast<std::streamsize>(length));
					if ((ss >> result))
						return result * sign;
					else
						abort_with_error(
							"Error parsing "sv, node_type::floating_point,
							"; '"sv, std::string_view{ chars, length }, "' could not be interpreted as a value"sv
						);
				}
				#else
				{
					auto parse_result = std::from_chars(chars, chars + length, result);
					switch (parse_result.ec)
					{
						case std::errc{}: //ok
							return result * sign;

						case std::errc::invalid_argument:
							abort_with_error(
								"Error parsing "sv, node_type::floating_point,
								"; '"sv, std::string_view{ chars, length }, "' could not be interpreted as a value"sv
							);
							break;

						case std::errc::result_out_of_range:
							abort_with_error(
								"Error parsing "sv, node_type::floating_point,
								"; '"sv, std::string_view{ chars, length }, "' is not representable in 64 bits"sv
							);
							break;

						default: //??
							abort_with_error(
								"Error parsing "sv, node_type::floating_point,
								"; an unspecified error occurred while trying to interpret '",
								std::string_view{ chars, length }, "' as a value"sv
							);
					}
				}
				#endif
				TOML_ERROR_CHECK({});
				TOML_UNREACHABLE;
			}

			#if !TOML_USE_STREAMS_FOR_FLOATS && TOML_LANG_HIGHER_THAN(0, 5, 0) // toml/issues/562

			[[nodiscard]]
			double parse_hex_float() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && *cp == U'0');

				const auto eof_check = [this]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error("Encountered EOF while parsing hexadecimal "sv, node_type::floating_point);
				};

				// '0'
				if (*cp != U'0')
					abort_with_error(
						"Encountered unexpected character while parsing hexadecimal "sv, node_type::floating_point,
						"; expected '0', saw '"sv, *cp, '\''
					);
				advance();
				eof_check();

				// 'x' or 'X'
				if (*cp != U'x' && *cp != U'X')
					abort_with_error(
						"Encountered unexpected character while parsing hexadecimal "sv, node_type::floating_point,
						"; expected 'x' or 'X', saw '"sv, *cp, '\''
					);
				advance();
				eof_check();

				TOML_ERROR_CHECK({});

				// consume value chars
				TOML_GCC_ATTR(uninitialized) char chars[23]; //23 = strlen("1.0123456789ABCDEFp+999")
				size_t length = {};
				const utf8_codepoint* prev = {};
				bool seen_decimal = false, seen_exponent_sign = false, seen_exponent = false;
				while (true)
				{
					if (!cp || is_value_terminator(*cp))
						break;

					if (*cp == U'_')
					{
						if (!prev || !is_hexadecimal_digit(*prev))
							abort_with_error(
								"Encountered unexpected character while parsing hexadecimal "sv,
								node_type::floating_point, "; underscores may only follow digits"sv
							);
						prev = cp;
						advance();
						TOML_ERROR_CHECK({});
						continue;
					}
					
					if (*cp == U'.')
					{
						if (seen_decimal)
							abort_with_error(
								"Encountered unexpected character while parsing hexadecimal "sv,
								node_type::floating_point, "; decimal points may appear only once"sv
							);
						if (seen_exponent)
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::floating_point,
								"; decimal points may not appear after exponents"sv
							);
						seen_decimal = true;
					}
					else if (*cp == U'p' || *cp == U'P')
					{
						if (seen_exponent)
							abort_with_error(
								"Encountered unexpected character while parsing hexadecimal "sv,
								node_type::floating_point, "; exponents may appear only once"sv
							);
						if (!seen_decimal)
							abort_with_error(
								"Encountered unexpected character while parsing hexadecimal "sv,
								node_type::floating_point, "; exponents may not appear before decimal points"sv
							);
						seen_exponent = true;
					}
					else if (*cp == U'+' || *cp == U'-')
					{
						if (!seen_exponent || !(*prev == U'p' || *prev == U'P'))
							abort_with_error(
								"Encountered unexpected character while parsing hexadecimal "sv,
								node_type::floating_point, "; exponent signs must immediately follow 'p'"sv
							);
						if (seen_exponent_sign)
							abort_with_error(
								"Encountered unexpected character while parsing hexadecimal "sv,
								node_type::floating_point, "; exponents signs may appear only once"sv
							);
						seen_exponent_sign = true;
					}
					else
					{
						if (!seen_exponent && !is_hexadecimal_digit(*cp))
							abort_with_error("Encountered unexpected character while parsing hexadecimal "sv,
								node_type::floating_point, "; expected hexadecimal digit, saw '"sv, *cp, '\''
							);
						else if (seen_exponent && !is_decimal_digit(*cp))
							abort_with_error("Encountered unexpected character while parsing hexadecimal "sv,
								node_type::floating_point, " exponent; expected decimal digit, saw '"sv, *cp, '\''
							);
					}

					if (length == sizeof(chars))
						abort_with_error(
							"Error parsing hexadecimal "sv, node_type::floating_point,
							"; exceeds maximum length of "sv, sizeof(chars), " characters"sv
						);
					chars[length++] = static_cast<char>(cp->bytes[0]);
					prev = cp;
					advance();

					TOML_ERROR_CHECK({});
				}
				if (prev && *prev == U'_')
				{
					eof_check();
					if (seen_exponent)
						abort_with_error(
							"Encountered unexpected character while parsing hexadecimal "sv,
							node_type::floating_point, " exponent; expected decimal digit, saw '"sv, *cp, '\''
						);
					else
						abort_with_error(
							"Encountered unexpected character while parsing hexadecimal "sv,
							node_type::floating_point, "; expected hexadecimal digit, saw '"sv, *cp, '\''
						);
				}

				TOML_ERROR_CHECK({});

				// convert to double
				TOML_GCC_ATTR(uninitialized) double result;
				auto parse_result = std::from_chars(chars, chars + length, result, std::chars_format::hex);
				switch (parse_result.ec)
				{
					case std::errc{}: //ok
						return result;

					case std::errc::invalid_argument:
						abort_with_error(
							"Error parsing hexadecimal "sv, node_type::floating_point,
							"; '"sv, std::string_view{ chars, length }, "' could not be interpreted as a value"sv
						);
						break;

					case std::errc::result_out_of_range:
						abort_with_error(
							"Error parsing hexadecimal "sv, node_type::floating_point,
							"; '"sv, std::string_view{ chars, length }, "' is not representable in 64 bits"sv
						);
						break;

					default: //??
						abort_with_error(
							"Error parsing hexadecimal "sv, node_type::floating_point,
							"; an unspecified error occurred while trying to interpret '",
							std::string_view{ chars, length }, "' as a value"sv
						);
				}
				TOML_ERROR_CHECK({});
				TOML_UNREACHABLE;
			}

			#endif //!TOML_USE_STREAMS_FOR_FLOATS && TOML_LANG_HIGHER_THAN(0, 5, 0)

			template <int base>
			[[nodiscard]]
			int64_t parse_integer() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp);
				using traits = parse_integer_traits<base>;

				const auto eof_check = [this]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error(
							"Encountered EOF while parsing "sv, traits::qualifier, ' ', node_type::integer
						);
				};

				[[maybe_unused]] int64_t sign = 1;
				if constexpr (traits::is_signed)
				{
					if (*cp == U'-')
					{
						sign = -1;
						advance();
					}
					else if(*cp == U'+')
						advance();
					eof_check();

					TOML_ERROR_CHECK({});
				}

				if constexpr (base == 10)
				{
					if (!traits::is_digit(*cp))
						abort_with_error(
							"Encountered unexpected character while parsing "sv, traits::qualifier, ' ',
							node_type::integer, "; expected expected "sv, traits::qualifier,
							" digit or sign, saw '"sv, *cp, '\''
						);
				}
				else
				{
					// '0'
					if (*cp != U'0')
						abort_with_error(
							"Encountered unexpected character while parsing "sv, traits::qualifier,
							' ', node_type::integer, "; expected '0', saw '"sv, *cp, '\''
						);
					advance();
					eof_check();

					// 'b', 'o', 'x'
					if (*cp != traits::prefix_codepoint)
						abort_with_error(
							"Encountered unexpected character while parsing "sv, traits::qualifier,
							' ', node_type::integer, "; expected '"sv, traits::prefix,
							"', saw '"sv, *cp, '\''
						);
					advance();
					eof_check();
				}

				TOML_ERROR_CHECK({});

				// consume value chars
				TOML_GCC_ATTR(uninitialized) char chars[traits::buffer_length];
				size_t length = {};
				const utf8_codepoint* prev = {};
				while (true)
				{
					if (!cp || is_value_terminator(*cp))
						break;

					if (*cp == U'_')
					{
						if (!prev || !traits::is_digit(*prev))
							abort_with_error(
								"Encountered unexpected character while parsing "sv, traits::qualifier,
								' ', node_type::integer, "; expected "sv, traits::qualifier, " digit, saw '_'"sv
							);
					}
					else
					{
						if (!traits::is_digit(*cp))
							abort_with_error(
								"Encountered unexpected character while parsing "sv, traits::qualifier,
								' ', node_type::integer, "; expected "sv, traits::qualifier,
								" digit, saw '"sv, *cp, '\''
							);
						if (length == sizeof(chars))
							abort_with_error(
								"Error parsing "sv, traits::qualifier, ' ', node_type::integer,
								"; exceeds maximum length of "sv, sizeof(chars), " characters"sv
							);
						chars[length++] = static_cast<char>(cp->bytes[0]);
					}

					prev = cp;
					advance();
					TOML_ERROR_CHECK({});
				}
				if (prev && *prev == U'_')
				{
					eof_check();
					abort_with_error(
						"Encountered unexpected character while parsing "sv, traits::qualifier,
						' ', node_type::integer, "; expected "sv, traits::qualifier, " digit, saw '_'"sv
					);
				}

				// check for leading zeroes
				if constexpr (base == 10)
				{
					if (chars[0] == '0')
						abort_with_error(
							"Error parsing "sv, traits::qualifier,
							' ', node_type::integer, "; leading zeroes are not allowed"sv
						);
				}
				
				TOML_ERROR_CHECK({});

				// single digits can be converted directly
				if (length == 1_sz)
				{
					if constexpr (base > 10)
					{
						return chars[0] >= 'A'
							? 10LL + static_cast<int64_t>(*cp - (*cp >= 'a' ? 'a' : 'A'))
							: static_cast<int64_t>(*cp - '0');
					}
					else
						return static_cast<int64_t>(chars[0] - '0');
				}

				// otherwise invoke charconv
				TOML_GCC_ATTR(uninitialized) uint64_t result;
				auto parse_result = std::from_chars(chars, chars + length, result, base);
				if constexpr (traits::is_signed)
				{
					if (parse_result.ec == std::errc{} && (
							(sign < 0 && result > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ull)
							|| (sign > 0 && result > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
						))
						parse_result.ec = std::errc::result_out_of_range;
				}
				else
				{
					if (parse_result.ec == std::errc{} &&
							result > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
						)
						parse_result.ec = std::errc::result_out_of_range;
				}
				switch (parse_result.ec)
				{
					case std::errc{}: //ok
						if constexpr (traits::is_signed)
							return static_cast<int64_t>(result) * sign;
						else
							return static_cast<int64_t>(result);

					case std::errc::invalid_argument:
						abort_with_error(
							"Error parsing "sv, traits::qualifier, ' ', node_type::integer,
							"; '"sv, std::string_view{ chars, length }, "' could not be interpreted as a value"sv
						);
						break;

					case std::errc::result_out_of_range:
						abort_with_error(
							"Error parsing "sv, traits::qualifier, ' ', node_type::integer,
							"; '"sv, std::string_view{ chars, length }, "' is not representable in 64 bits"sv
						);
						break;

					default: //??
						abort_with_error(
							"Error parsing "sv, traits::qualifier, ' ', node_type::integer,
							"; an unspecified error occurred while trying to interpret '",
							std::string_view{ chars, length }, "' as a value"sv
						);
				}

				TOML_ERROR_CHECK({});
				TOML_UNREACHABLE;
			}

			[[nodiscard]]
			date parse_date(bool part_of_datetime = false) TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && is_decimal_digit(*cp));

				const auto type = part_of_datetime ? node_type::date_time : node_type::date;
				const auto eof_check = [this, type]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error(
							"Encountered EOF while parsing "sv, type);
				};

				// "YYYY"
				TOML_GCC_ATTR(uninitialized) uint32_t year_digits[4];
				if (!consume_digit_sequence(year_digits))
				{
					eof_check();
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected 4-digit year, saw '"sv, *cp, '\''
					);
				}
				const auto year = year_digits[3]
					+ year_digits[2] * 10u
					+ year_digits[1] * 100u
					+ year_digits[0] * 1000u;
				const auto is_leap_year = (year % 4u == 0u) && ((year % 100u != 0u) || (year % 400u == 0u));

				// '-'
				eof_check();
				if (*cp != U'-')
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected '-', saw '"sv, *cp, '\''
					);
				advance();
				TOML_ERROR_CHECK({});

				// "MM"
				TOML_GCC_ATTR(uninitialized) uint32_t month_digits[2];
				if (!consume_digit_sequence(month_digits))
				{
					eof_check();
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected 2-digit month, saw '"sv, *cp, '\''
					);
				}
				const auto month = month_digits[1] + month_digits[0] * 10u;
				if (month == 0u || month > 12u)
					abort_with_error(
						"Error parsing "sv, type,
						"; expected month between 1 and 12 (inclusive), saw "sv, month
					);
				const auto max_days_in_month =
					month == 2u
					? (is_leap_year ? 29u : 28u)
					: (month == 4u || month == 6u || month == 9u || month == 11u ? 30u : 31u)
				;

				// '-'
				eof_check();
				if (*cp != U'-')
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected '-', saw '"sv, *cp, '\''
					);
				advance();
				TOML_ERROR_CHECK({});

				// "DD"
				TOML_GCC_ATTR(uninitialized) uint32_t day_digits[2];
				if (!consume_digit_sequence(day_digits))
				{
					eof_check();
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected 2-digit day, saw '"sv, *cp, '\''
					);
				}
				const auto day = day_digits[1] + day_digits[0] * 10u;
				if (day == 0u || day > max_days_in_month)
					abort_with_error("Error parsing "sv, type,
						"; expected day between 1 and "sv, max_days_in_month, " (inclusive), saw "sv, day
					);

				if (!part_of_datetime)
				{
					if (cp && !is_value_terminator(*cp))
						abort_with_error(
							"Encountered unexpected character while parsing "sv, type,
							"; expected value-terminator, saw '"sv, *cp, '\''
						);
				}

				TOML_ERROR_CHECK({});

				return
				{
					static_cast<uint16_t>(year),
					static_cast<uint8_t>(month),
					static_cast<uint8_t>(day)
				};
			}

			[[nodiscard]]
			time parse_time(bool part_of_datetime = false) TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && is_decimal_digit(*cp));

				const auto type = part_of_datetime ? node_type::date_time : node_type::date;
				const auto eof_check = [this, type]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error("Encountered EOF while parsing "sv, type);
				};

				// "HH"
				TOML_GCC_ATTR(uninitialized) uint32_t digits[2];
				if (!consume_digit_sequence(digits))
				{
					eof_check();
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected 2-digit hour, saw '"sv, *cp, '\''
					);
				}
				const auto hour = digits[1] + digits[0] * 10u;
				if (hour > 23u)
					abort_with_error(
						"Error parsing "sv, type,
						"; expected hour between 0 to 59 (inclusive), saw "sv, hour
					);

				// ':'
				eof_check();
				if (*cp != U':')
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected ':', saw '"sv, *cp, '\''
					);
				advance();
				TOML_ERROR_CHECK({});

				// "MM"
				if (!consume_digit_sequence(digits))
				{
					eof_check();
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected 2-digit minute, saw '"sv,
						*cp, '\''
					);
				}
				const auto minute = digits[1] + digits[0] * 10u;
				if (minute > 59u)
					abort_with_error(
						"Error parsing "sv, type,
						"; expected minute between 0 and 59 (inclusive), saw "sv, minute
					);

				auto time = ::toml::time{
					static_cast<uint8_t>(hour),
					static_cast<uint8_t>(minute),
				};

				if constexpr (!TOML_LANG_HIGHER_THAN(0, 5, 0)) // toml/issues/671
				{
					// ':'
					eof_check();
					if (*cp != U':')
						abort_with_error(
							"Encountered unexpected character while parsing "sv, type,
							"; expected ':', saw '"sv, *cp, '\''
						);
					advance();
				}
				else
				{
					if (cp
						&& !is_value_terminator(*cp)
						&& *cp != U':'
						&& (!part_of_datetime || (*cp != U'+' && *cp != U'-' && *cp != U'Z' && *cp != U'z')))
						abort_with_error(
							"Encountered unexpected character while parsing "sv, type,
							"; expected ':'"sv, (part_of_datetime ? ", offset"sv : ""sv),
							" or value-terminator, saw '"sv, *cp, '\''
						);
					TOML_ERROR_CHECK({});
					if (!cp || *cp != U':')
						return time;

					// ':'
					advance();
				}

				TOML_ERROR_CHECK({});

				// "SS"
				if (!consume_digit_sequence(digits))
				{
					eof_check();
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected 2-digit second, saw '"sv,
						*cp, '\''
					);
				}
				const auto second = digits[1] + digits[0] * 10u;
				if (second > 59u)
					abort_with_error(
						"Error parsing "sv, type,
						"; expected second between 0 and 59 (inclusive), saw "sv, second
					);
				time.second = static_cast<uint8_t>(second);

				//early exit here if the fractional is omitted
				if (cp
					&& !is_value_terminator(*cp)
					&& *cp != U'.'
					&& (!part_of_datetime || (*cp != U'+' && *cp != U'-' && *cp != U'Z' && *cp != U'z')))
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected fractional"sv,
						(part_of_datetime ? ", offset"sv : ""sv),
						" or value-terminator, saw '"sv, *cp, '\''
					);
				TOML_ERROR_CHECK({});
				if (!cp || *cp != U'.')
					return time;

				// '.'
				advance();
				eof_check();
				TOML_ERROR_CHECK({});

				// ".FFFFFFFFF"
				static constexpr auto max_fractional_digits = 9_sz;
				TOML_GCC_ATTR(uninitialized) uint32_t fractional_digits[max_fractional_digits];
				auto digit_count = consume_variable_length_digit_sequence(fractional_digits);
				if (!digit_count)
					abort_with_error(
						"Encountered unexpected character while parsing "sv, type,
						"; expected fractional digits, saw '"sv,
						*cp, '\''
					);
				if (digit_count == max_fractional_digits && cp && is_decimal_digit(*cp))
					abort_with_error(
						"Error parsing "sv, type,
						"Fractional component exceeds maximum precision of "sv, max_fractional_digits
					);

				uint32_t value = 0u;
				uint32_t place = 1u;
				for (auto i = digit_count; i --> 0_sz;)
				{
					value += fractional_digits[i] * place;
					place *= 10u;
				}
				for (auto i = digit_count; i < max_fractional_digits; i++) //implicit zeros
					value *= 10u;
				time.nanosecond = value;

				return time;
			}

			[[nodiscard]]
			date_time parse_date_time() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && is_decimal_digit(*cp));

				const auto eof_check = [this]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error("Encountered EOF while parsing "sv, node_type::date_time);
				};

				// "YYYY-MM-DD"
				auto date = parse_date(true);
				TOML_ERROR_CHECK({});

				// ' ' or 'T'
				eof_check();
				if (*cp != U' ' && *cp != U'T' && *cp != U't')
					abort_with_error(
						"Encountered unexpected character while parsing "sv, node_type::date_time,
						"; expected space or 'T', saw '"sv, *cp, '\''
					);
				advance();

				// "HH:MM:SS.fractional"
					
				auto time = parse_time(true);
				TOML_ERROR_CHECK({});

				// offset
				std::optional<time_offset> offset;
				if (cp)
				{
					// zero offset ("Z")
					if (*cp == U'Z' || *cp == U'z')
					{
						offset.emplace(time_offset{});
						advance();
					}

					// explicit offset ("+/-HH:MM")
					else if (*cp == U'+' || *cp == U'-')
					{
						// sign
						int sign = *cp == U'-' ? -1 : 1;
						advance();
						eof_check();

						// "HH"
						TOML_GCC_ATTR(uninitialized) int digits[2];
						if (!consume_digit_sequence(digits))
						{
							eof_check();
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::date_time,
								" offset; expected 2-digit hour, saw '"sv, *cp, '\''
							);
						}
						const auto hour = digits[1] + digits[0] * 10;
						if (hour > 23)
							abort_with_error(
								"Error parsing "sv, node_type::date_time,
								" offset; expected hour between 0 and 23 (inclusive), saw "sv, hour 
							);


						// ':'
						eof_check();
						if (*cp != U':')
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::date_time,
								"offset; expected ':', saw '"sv, *cp, '\''
							);
						advance();

						// "MM"
						if (!consume_digit_sequence(digits))
						{
							eof_check();
							abort_with_error(
								"Encountered unexpected character while parsing "sv, node_type::date_time,
								" offset; expected 2-digit minute, saw '"sv, *cp, '\''
							);
						}
						const auto minute = digits[1] + digits[0] * 10;
						if (minute > 59)
							abort_with_error(
								"Error parsing "sv, node_type::date_time,
								" offset; expected minute between 0 and 59 (inclusive), saw "sv, hour
							);

						offset.emplace(time_offset{ static_cast<int16_t>((hour * 60 + minute) * sign) });
					}
				}

					
				if (cp && !is_value_terminator(*cp))
					abort_with_error(
						"Encountered unexpected character while parsing "sv, node_type::date_time,
						"; expected value-terminator, saw '"sv, *cp, '\''
					);

				TOML_ERROR_CHECK({});
				return {
					date,
					time,
					offset
				};
			}

			// TOML_DISABLE_SWITCH_WARNINGS
			// TOML_DISABLE_INIT_WARNINGS
			TOML_POP_WARNINGS 

			[[nodiscard]]
			inline std::unique_ptr<toml::array> parse_array() TOML_MAY_THROW;

			[[nodiscard]]
			inline std::unique_ptr<toml::table> parse_inline_table() TOML_MAY_THROW;

			[[nodiscard]]
			std::unique_ptr<node> parse_value() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && !is_value_terminator(*cp));

				const auto begin_pos = cp->position;
				std::unique_ptr<node> val;

				do
				{
					// detect the value type and parse accordingly,
					// starting with value types that can be detected
					// unambiguously from just one character.

					// strings
					if (is_string_delimiter(*cp))
						val = std::make_unique<value<string>>(parse_string());

					// bools
					else if (*cp == U't' || *cp == U'f')
						val = std::make_unique<value<bool>>(parse_bool());

					// arrays
					else if (*cp == U'[')
					{
						val = parse_array();
						if constexpr (!TOML_LANG_HIGHER_THAN(0, 5, 0)) // toml/issues/665
						{
							// arrays must be homogeneous in toml 0.5.0 and earlier
							if (!val->reinterpret_as<array>()->is_homogeneous())
								TOML_ERROR(
									"Arrays cannot contain values of different types in TOML 0.5.0 and earlier.",
									begin_pos,
									reader.source_path()
								);
						}
					}

					// inline tables
					else if (*cp == U'{')
						val = parse_inline_table();

					// inf or nan
					else if (*cp == U'i' || *cp == U'n')
						val = std::make_unique<value<double>>(parse_inf_or_nan());

					// underscores at the beginning
					else if (*cp == U'_')
						abort_with_error("Values may not begin with underscores."sv);

					TOML_ERROR_CHECK({});
					if (val)
						break;

					// value types from here down require more than one character
					// to unambiguously identify, so scan ahead a bit.
					enum value_string_traits : int
					{
						has_nothing = 0,
						has_digits = 1,
						has_e = 2,
						has_t = 4,
						has_z = 8,
						has_colon = 16,
						has_plus = 32,
						has_minus = 64,
						has_sign = has_plus | has_minus,
						has_dot = 128,
						has_space = 256
					};
					int traits = has_nothing;
					TOML_GCC_ATTR(uninitialized) char32_t chars[utf8_buffered_reader::max_history_length];
					size_t char_count = {}, advance_count = {};
					bool eof_while_scanning = false;
					const auto scan = [&]() TOML_MAY_THROW
					{
						while (advance_count < utf8_buffered_reader::max_history_length)
						{
							if (!cp || is_value_terminator(*cp))
							{
								eof_while_scanning = !cp;
								break;
							}

							if (*cp != U'_')
							{
								chars[char_count++] = *cp;
								switch (*cp)
								{
									case U'E': [[fallthrough]];
									case U'e': traits |= has_e; break;
									case U'T': traits |= has_t; break;
									case U'Z': traits |= has_z; break;
									case U'+': traits |= has_plus; break;
									case U'-': traits |= has_minus; break;
									case U'.': traits |= has_dot; break;
									case U':': traits |= has_colon; break;
									default: if (is_decimal_digit(*cp)) traits |= has_digits;
								}
							}
							advance();
							advance_count++;
							TOML_ERROR_CHECK();
						}
					};
					scan();
					TOML_ERROR_CHECK({});

					//force further scanning if this could have been a date-time with a space instead of a T
					if (char_count == 10_sz
						&& traits == (has_digits | has_minus)
						&& chars[4] == U'-'
						&& chars[7] == U'-'
						&& cp
						&& *cp == U' ')
					{
						const auto pre_advance_count = advance_count;
						const auto pre_scan_traits = traits;
						chars[char_count++] = *cp;
						traits |= has_space;

						const auto backpedal = [&]() noexcept
						{
							go_back(advance_count - pre_advance_count);
							advance_count = pre_advance_count;
							traits = pre_scan_traits;
							char_count = 10_sz;
						};

						advance();
						advance_count++;
						TOML_ERROR_CHECK({});

						if (!cp || !is_decimal_digit(*cp))
							backpedal();
						else
						{
							chars[char_count++] = *cp;

							advance();
							advance_count++;
							TOML_ERROR_CHECK({});

							scan();
							TOML_ERROR_CHECK({});

							if (char_count == 12_sz)
								backpedal();
						}
					}

					//set the reader back to where we started
					go_back(advance_count);
					if (char_count < utf8_buffered_reader::max_history_length - 1_sz)
						chars[char_count] = U'\0';

					// if after scanning ahead we still only have one value character,
					// the only valid value type is an integer.
					if (char_count == 1_sz)
					{
						if ((traits & has_digits))
						{
							val = std::make_unique<value<int64_t>>(static_cast<int64_t>(chars[0] - U'0'));
							advance(); //skip the digit
							break;
						}

						//anything else would be ambiguous.
						abort_with_error(
							eof_while_scanning
								? "Encountered EOF while parsing value"sv
								: "Could not determine value type"sv
						);
					}

					// now things that can be identified from two or more characters
					TOML_ERROR_CHECK({});
					TOML_ASSERT(char_count >= 2_sz);

					// numbers that begin with a sign
					const auto begins_with_sign = chars[0] == U'+' || chars[0] == U'-';
					const auto begins_with_digit = is_decimal_digit(chars[0]);
					if (begins_with_sign)
					{
						if (char_count == 2_sz && is_decimal_digit(chars[1]))
						{
							val = std::make_unique<value<int64_t>>(
								static_cast<int64_t>(chars[1] - U'0')
								* (chars[1] == U'-' ? -1LL : 1LL)
							);
							advance(); //skip the sign
							advance(); //skip the digit
						}

						else if (chars[1] == U'i' || chars[1] == U'n')
							val = std::make_unique<value<double>>(parse_inf_or_nan());
						else if (is_decimal_digit(chars[1]) && (chars[2] == U'.' || chars[2] == U'e' || chars[2] == U'E'))
							val = std::make_unique<value<double>>(parse_float());
					}

					// numbers that begin with 0
					else if (chars[0] == U'0')
					{
						switch (chars[1])
						{
							case U'E': [[fallthrough]];
							case U'e': [[fallthrough]];
							case U'.': val = std::make_unique<value<double>>(parse_float()); break;
							case U'b': val = std::make_unique<value<int64_t>>(parse_integer<2>()); break;
							case U'o': val = std::make_unique<value<int64_t>>(parse_integer<8>()); break;
							case U'X': [[fallthrough]];
							case U'x':
							{
								for (size_t i = char_count; i-- > 2_sz;)
								{
									if (chars[i] == U'p' || chars[i] == U'P')
									{
										#if !TOML_LANG_HIGHER_THAN(0, 5, 0) // toml/issues/562
											TOML_ERROR(
												"Hexadecimal floating-point values are not supported "
												"in TOML 0.5.0 and earlier.",
												begin_pos,
												reader.source_path()
											);
										#elif TOML_USE_STREAMS_FOR_FLOATS
											TOML_ERROR(
												"Hexadecimal floating-point values are not "
												"supported when streams are used to interpret floats "
												"(TOML_USE_STREAMS_FOR_FLOATS = 1).",
												begin_pos,
												reader.source_path()
											);
										#else
											val = std::make_unique<value<double>>(parse_hex_float());
											break;
										#endif
									}
								}
								TOML_ERROR_CHECK({});
								if (val)
									break;

								val = std::make_unique<value<int64_t>>(parse_integer<16>());
								break;
							}
						}
					}

					TOML_ERROR_CHECK({});
					if (val)
						break;

					// from here down it can only be date-times, floats and integers.
					if (begins_with_digit)
					{
						switch (traits)
						{
							// 100
							case has_digits:
								val = std::make_unique<value<int64_t>>(parse_integer<10>());
								break;

							// 1e1
							// 1e-1
							// 1e+1
							// 1.0
							// 1.0e1
							// 1.0e-1
							// 1.0e+1
							case has_digits | has_e:						[[fallthrough]];
							case has_digits | has_e | has_minus:			[[fallthrough]];
							case has_digits | has_e | has_plus:				[[fallthrough]];
							case has_digits | has_dot:						[[fallthrough]];
							case has_digits | has_dot | has_e:				[[fallthrough]];
							case has_digits | has_dot | has_e | has_minus:	[[fallthrough]];
							case has_digits | has_dot | has_e | has_plus:
								val = std::make_unique<value<double>>(parse_float());
								break;

							// HH:MM
							// HH:MM:SS
							// HH:MM:SS.FFFFFF
							case has_digits | has_colon:			[[fallthrough]];
							case has_digits | has_colon | has_dot:
								val = std::make_unique<value<time>>(parse_time());
								break;

							// YYYY-MM-DD
							case has_digits | has_minus:
								val = std::make_unique<value<date>>(parse_date());
								break;

							// YYYY-MM-DDTHH:MM
							// YYYY-MM-DDTHH:MM-HH:MM
							// YYYY-MM-DDTHH:MM+HH:MM
							// YYYY-MM-DD HH:MM
							// YYYY-MM-DD HH:MM-HH:MM
							// YYYY-MM-DD HH:MM+HH:MM
							// YYYY-MM-DDTHH:MM:SS
							// YYYY-MM-DDTHH:MM:SS-HH:MM
							// YYYY-MM-DDTHH:MM:SS+HH:MM
							// YYYY-MM-DD HH:MM:SS
							// YYYY-MM-DD HH:MM:SS-HH:MM
							// YYYY-MM-DD HH:MM:SS+HH:MM
							case has_digits | has_minus | has_colon | has_t:		[[fallthrough]];
							case has_digits | has_sign  | has_colon | has_t:		[[fallthrough]];
							case has_digits | has_minus | has_colon | has_space:	[[fallthrough]];
							case has_digits | has_sign  | has_colon | has_space:	[[fallthrough]];
							// YYYY-MM-DDTHH:MM:SS.FFFFFF
							// YYYY-MM-DDTHH:MM:SS.FFFFFF-HH:MM
							// YYYY-MM-DDTHH:MM:SS.FFFFFF+HH:MM
							// YYYY-MM-DD HH:MM:SS.FFFFFF
							// YYYY-MM-DD HH:MM:SS.FFFFFF-HH:MM
							// YYYY-MM-DD HH:MM:SS.FFFFFF+HH:MM
							case has_digits | has_minus | has_colon | has_dot | has_t:		[[fallthrough]];
							case has_digits | has_sign  | has_colon | has_dot | has_t:		[[fallthrough]];
							case has_digits | has_minus | has_colon | has_dot | has_space:	[[fallthrough]];
							case has_digits | has_sign  | has_colon | has_dot | has_space:	[[fallthrough]];
							// YYYY-MM-DDTHH:MMZ
							// YYYY-MM-DD HH:MMZ
							// YYYY-MM-DDTHH:MM:SSZ
							// YYYY-MM-DD HH:MM:SSZ
							// YYYY-MM-DDTHH:MM:SS.FFFFFFZ
							// YYYY-MM-DD HH:MM:SS.FFFFFFZ
							case has_digits | has_minus | has_colon | has_z | has_t:				[[fallthrough]];
							case has_digits | has_minus | has_colon | has_z | has_space:			[[fallthrough]];
							case has_digits | has_minus | has_colon | has_dot | has_z | has_t:		[[fallthrough]];
							case has_digits | has_minus | has_colon | has_dot | has_z | has_space:
								val = std::make_unique<value<date_time>>(parse_date_time());
								break;
						}
					}
					else if (begins_with_sign)
					{
						switch (traits)
						{
							// +100
							// -100
							case has_digits | has_minus:	[[fallthrough]];
							case has_digits | has_plus:
								val = std::make_unique<value<int64_t>>(parse_integer<10>());
								break;

							// +1e1
							// +1.0
							// +1.0e1
							// +1.0e+1
							// +1.0e-1
							// -1.0e+1
							case has_digits | has_e | has_plus:				[[fallthrough]];
							case has_digits | has_dot | has_plus:			[[fallthrough]];
							case has_digits | has_dot | has_e | has_plus:	[[fallthrough]];
							case has_digits | has_dot | has_e | has_sign:	[[fallthrough]];
							// -1e1
							// -1.0
							// -1.0e1
							// -1.0e-1
							case has_digits | has_e | has_minus:			[[fallthrough]];
							case has_digits | has_dot | has_minus:			[[fallthrough]];
							case has_digits | has_dot | has_e | has_minus:
								val = std::make_unique<value<double>>(parse_float());
								break;
						}
					}
				}
				while (false);

				if (!val)
				{
					abort_with_error("Could not determine value type"sv);
					TOML_ERROR_CHECK({});
				}

				val->source_ = { begin_pos, current_position_or_assumed_next(), reader.source_path() };
				return val;
			}

			struct key final
			{
				std::vector<string> segments;
			};

			[[nodiscard]]
			key parse_key() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && (is_bare_key_start_character(*cp) || is_string_delimiter(*cp)));

				key key;

				while (true)
				{
					if (!cp)
						abort_with_error("Encountered EOF while parsing key"sv);

					// bare_key_segment
					#if TOML_LANG_HIGHER_THAN(0, 5, 0) // toml/issues/687
					if (is_unicode_combining_mark(*cp))
						abort_with_error(
							"Encountered unexpected character while parsing key; expected bare key starting character "
							"or string delimiter, saw combining mark '"sv, *cp, '\''
						);
					else
					#endif
					if (is_bare_key_start_character(*cp))
						key.segments.push_back(parse_bare_key_segment());

					// "quoted key segment"
					else if (is_string_delimiter(*cp))
						key.segments.push_back(parse_string());

					// ???
					else
						abort_with_error(
							"Encountered unexpected character while parsing key; expected bare key "
							"starting character or string delimiter, saw '"sv, *cp, '\''
						);
						
					consume_leading_whitespace();

					// eof or no more key to come
					if (!cp)
						break;
					if (*cp != U'.')
					{
						if (recording)
							stop_recording(1_sz);
						break;
					}

					// was a dotted key, so go around again to consume the next segment
					TOML_ASSERT(*cp == U'.');
					advance();
					consume_leading_whitespace();
				}
				TOML_ERROR_CHECK({});
				return key;
			}

			struct key_value_pair final
			{
				parser::key key;
				std::unique_ptr<node> value;
			};

			[[nodiscard]]
			key_value_pair parse_key_value_pair() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});

				const auto eof_check = [this]() TOML_MAY_THROW
				{
					TOML_ERROR_CHECK();
					if (!cp)
						abort_with_error("Encountered EOF while parsing key-value pair"sv);
				};

				// get the key
				TOML_ASSERT(cp && (is_string_delimiter(cp->value) || is_bare_key_start_character(cp->value)));
				start_recording();
				auto key = parse_key(); //parse_key() calls stop_recording()

				// skip past any whitespace that followed the key
				consume_leading_whitespace();
				eof_check();

				// consume the '='
				if (*cp != U'=')
					abort_with_error(
						"Encountered unexpected character while parsing key-value pair; "
						"expected '=', saw '"sv, *cp, '\''
					);
				advance();

				// skip past any whitespace that followed the '='
				consume_leading_whitespace();
				eof_check();

				// get the value
				if (is_value_terminator(*cp))
					abort_with_error(
						"Encountered unexpected character while parsing key-value pair; "
						"expected value, saw '"sv, *cp, '\''
					);
				TOML_ERROR_CHECK({});
				return { std::move(key), parse_value() };
			}

			[[nodiscard]]
			table* parse_table_header() TOML_MAY_THROW
			{
				TOML_ERROR_CHECK({});
				TOML_ASSERT(cp && *cp == U'[');

				const auto header_begin_pos = cp->position;
				source_position header_end_pos;
				key key;
				bool is_array = false;

				//parse header
				{
					const auto eof_check = [this]() TOML_MAY_THROW
					{
						TOML_ERROR_CHECK();
						if (!cp)
							abort_with_error("Encountered EOF while parsing table header"sv);
					};

					// skip first '['
					advance();
					eof_check();

					// skip second '[' (if present)
					if (*cp == U'[')
					{
						advance();
						eof_check();
						is_array = true;
					}

					// skip past any whitespace that followed the '['
					consume_leading_whitespace();
					eof_check();

					// sanity-check the key start
					#if TOML_LANG_HIGHER_THAN(0, 5, 0) // toml/issues/687
					if (is_unicode_combining_mark(*cp))
						abort_with_error(
							"Encountered unexpected character while parsing table key; "
							"expected bare key starting character or string delimiter, saw combining mark '"sv, *cp, '\''
						);
					else
					#endif
					if (!is_bare_key_start_character(cp->value) && !is_string_delimiter(cp->value))
					{
						abort_with_error(
							"Encountered unexpected character while parsing table key; "
							"expected bare key starting character or string delimiter, saw '"sv, *cp, '\''
						);
					}
					TOML_ERROR_CHECK({});

					// get the actual key
					start_recording();
					key = parse_key(); //parse_key() calls stop_recording()
					TOML_ERROR_CHECK({});

					// skip past any whitespace that followed the key
					consume_leading_whitespace();
					eof_check();

					// consume the first closing ']'
					if (*cp != U']')
						abort_with_error(
							"Encountered unexpected character while parsing table"sv,
							(is_array ? " array"sv : ""sv), " header; expected ']', saw '"sv, *cp, '\''
						);
					advance();

					// consume the second closing ']'
					if (is_array)
					{
						eof_check();

						if (*cp != U']')
							abort_with_error(
								"Encountered unexpected character while parsing table array header; "
								"expected ']', saw '"sv, *cp, '\''
							);
						advance();
					}
					header_end_pos = current_position_or_assumed_next();

					// handle the rest of the line after the header
					consume_leading_whitespace();
					if (cp)
					{
						if (!consume_comment() && !consume_line_break())
							abort_with_error(
								"Encountered unexpected character after table"sv, (is_array ? " array"sv : ""sv),
								" header; expected a comment or whitespace, saw '"sv, *cp, '\''
							);
					}
				}
				TOML_ERROR_CHECK({});
				TOML_ASSERT(!key.segments.empty());

				// check if each parent is a table/table array, or can be created implicitly as a table.
				auto parent = &root;
				for (size_t i = 0; i < key.segments.size() - 1_sz; i++)
				{
					auto child = parent->get(key.segments[i]);
					if (!child)
					{
						child = parent->values.emplace(
							key.segments[i],
							std::make_unique<table>()
						).first->second.get();
						implicit_tables.push_back(child->reinterpret_as<table>());
						child->source_ = { header_begin_pos, header_end_pos, reader.source_path() };
						parent = child->reinterpret_as<table>();
					}
					else if (child->is_table())
					{
						parent = child->reinterpret_as<table>();
					}
					else if (child->is_array() && find(table_arrays, child->reinterpret_as<array>()))
					{
						// table arrays are a special case;
						// the spec dictates we select the most recently declared element in the array.
						TOML_ASSERT(!child->reinterpret_as<array>()->values.empty());
						TOML_ASSERT(child->reinterpret_as<array>()->values.back()->is_table());
						parent = child->reinterpret_as<array>()->values.back()->reinterpret_as<table>();
					}
					else
					{
						if (!is_array && child->type() == node_type::table)
							abort_with_error("Attempt to redefine existing table '"sv, recording_buffer, '\'');
						else
							abort_with_error(
								"Attempt to redefine existing "sv, child->type(),
								" '"sv, recording_buffer,
								"' as "sv, is_array ? "array-of-tables"sv : "table"sv
							);
					}
				}
				TOML_ERROR_CHECK({});

				// check the last parent table for a node matching the last key.
				// if there was no matching node, then sweet;
				// we can freely instantiate a new table/table array.
				auto matching_node = parent->get(key.segments.back());
				if (!matching_node)
				{
					// if it's an array we need to make the array and it's first table element,
					// set the starting regions, and return the table element
					if (is_array)
					{
						auto tab_arr = parent->values.emplace(key.segments.back(),std::make_unique<array>())
							.first->second->reinterpret_as<array>();
						table_arrays.push_back(tab_arr);
						tab_arr->source_ = { header_begin_pos, header_end_pos, reader.source_path() };
						
						tab_arr->values.push_back(std::make_unique<table>());
						tab_arr->values.back()->source_ = { header_begin_pos, header_end_pos, reader.source_path() };
						return tab_arr->values.back()->reinterpret_as<table>();
					}

					//otherwise we're just making a table
					else
					{
						auto tab = parent->values.emplace(key.segments.back(),std::make_unique<table>())
							.first->second->reinterpret_as<table>();
						tab->source_ = { header_begin_pos, header_end_pos, reader.source_path() };
						return tab;
					}
				}

				// if there was already a matching node some sanity checking is necessary;
				// this is ok if we're making an array and the existing element is already an array (new element)
				// or if we're making a table and the existing element is an implicitly-created table (promote it),
				// otherwise this is a redefinition error.
				else
				{
					if (is_array && matching_node->is_array() && find(table_arrays, matching_node->reinterpret_as<array>()))
					{
						auto tab_arr = matching_node->reinterpret_as<array>();
						tab_arr->values.push_back(std::make_unique<table>());
						tab_arr->values.back()->source_ = { header_begin_pos, header_end_pos, reader.source_path() };
						return tab_arr->values.back()->reinterpret_as<table>();
					}

					else if (!is_array
						&& matching_node->is_table()
						&& !implicit_tables.empty())
					{
						auto tbl = matching_node->reinterpret_as<table>();
						const auto idx = find(implicit_tables, tbl);
						if (idx)
						{
							implicit_tables.erase(implicit_tables.cbegin() + static_cast<ptrdiff_t>(*idx));
							tbl->source_.begin = header_begin_pos;
							tbl->source_.end = header_end_pos;
							return tbl;
						}
					}

					//if we get here it's a redefinition error.
					if (!is_array && matching_node->type() == node_type::table)
						abort_with_error("Attempt to redefine existing table '"sv, recording_buffer, '\'');
					else
						abort_with_error(
							"Attempt to redefine existing "sv, matching_node->type(),
							" '"sv, recording_buffer,
							"' as "sv, is_array ? "array-of-tables"sv : "table"sv
						);
				}
				TOML_ERROR_CHECK({});
				TOML_UNREACHABLE;
			}

			void parse_key_value_pair_and_insert(table* tab) TOML_MAY_THROW
			{
				TOML_ERROR_CHECK();

				auto kvp = parse_key_value_pair();
				TOML_ERROR_CHECK();
				TOML_ASSERT(kvp.key.segments.size() >= 1_sz);

				if (kvp.key.segments.size() > 1_sz)
				{
					for (size_t i = 0; i < kvp.key.segments.size() - 1_sz; i++)
					{
						auto child = tab->get(kvp.key.segments[i]);
						if (!child)
						{
							child = tab->values.emplace(
								std::move(kvp.key.segments[i]),
								std::make_unique<table>()
							).first->second.get();
							dotted_key_tables.push_back(child->reinterpret_as<table>());
							dotted_key_tables.back()->inline_ = true;
							child->source_ = kvp.value->source_;
						}
						else if (!child->is_table() || !find(dotted_key_tables, child->reinterpret_as<table>()))
						{
							abort_with_error("Attempt to redefine "sv, child->type(), " as dotted key-value pair"sv);
						}
						else
							child->source_.end = kvp.value->source_.end;
						TOML_ERROR_CHECK();
						tab = child->reinterpret_as<table>();
					}
				}

				if (auto conflicting_node = tab->get(kvp.key.segments.back()))
				{
					if (conflicting_node->type() == kvp.value->type())
						abort_with_error("Attempt to redefine "sv, conflicting_node->type(), " '"sv, recording_buffer, '\'');
					else
						abort_with_error(
							"Attempt to redefine "sv, conflicting_node->type(),
							" '"sv, recording_buffer,
							"' as "sv, kvp.value->type()
						);
				}

				TOML_ERROR_CHECK();
				tab->values.emplace(
					std::move(kvp.key.segments.back()),
					std::move(kvp.value)
				);
			}

			void parse_document() TOML_MAY_THROW
			{
				TOML_ASSERT(cp);
					
				table* current_table = &root;

				do
				{
					TOML_ERROR_CHECK();

					// leading whitespace, line endings, comments
					if (consume_leading_whitespace()
						|| consume_line_break()
						|| consume_comment())
						continue;

					// [tables]
					// [[table array]]
					else if (*cp == U'[')
					{
						current_table = parse_table_header();
					}

					// bare_keys
					// dotted.keys
					// "quoted keys"
					#if TOML_LANG_HIGHER_THAN(0, 5, 0) // toml/issues/687
					else if (is_unicode_combining_mark(*cp))
						abort_with_error(
							"Encountered unexpected character while parsing key; "
							"expected bare key starting character or string delimiter, saw combining mark '"sv, *cp, '\''
						);
					#endif
					else if (is_bare_key_start_character(cp->value) || is_string_delimiter(cp->value))
					{
						parse_key_value_pair_and_insert(current_table);

						// handle the rest of the line after the kvp
						// (this is not done in parse_key_value_pair() because that function is also used for inline tables)
						consume_leading_whitespace();
						if (cp)
						{
							if (!consume_comment() && !consume_line_break())
								abort_with_error(
									"Encountered unexpected character after key-value pair; "
									"expected a comment or whitespace, saw '"sv, *cp, '\''
								);
						}
					}

					else // ??
						abort_with_error(
							"Encountered unexpected character while parsing top level of document; "
							"expected keys, tables, whitespace or comments, saw '"sv, *cp, '\''
						);

				}
				while (cp);

				auto eof_pos = current_position_or_assumed_next();
				eof_pos.column++;
				root.source_.end = eof_pos;
				if (current_table
					&& current_table != &root
					&& current_table->source_.end <= current_table->source_.begin)
					current_table->source_.end = eof_pos;
			}

			static void update_region_ends(node& nde) noexcept
			{
				const auto type = nde.type();
				if (type > node_type::array)
					return;

				if (type == node_type::table)
				{
					auto& tbl = *nde.reinterpret_as<table>();
					if (tbl.inline_) //inline tables (and all their inline descendants) are already correctly terminated
						return;

					auto end = nde.source_.end;
					for (auto& [k, v] : tbl.values)
					{
						update_region_ends(*v);
						if (end < v->source_.end)
							end = v->source_.end;
					}
				}
				else //arrays
				{
					auto& arr = *nde.reinterpret_as<array>();
					auto end = nde.source_.end;
					for (auto& v : arr.values)
					{
						update_region_ends(*v);
						if (end < v->source_.end)
							end = v->source_.end;
					}
					nde.source_.end = end;
				}
			}

		public:

			parser(utf8_reader_interface&& reader_) TOML_MAY_THROW
				: reader{ reader_ }
			{
				root.source_ = { prev_pos, prev_pos, reader.source_path() };

				cp = reader.read_next();

				#if !TOML_EXCEPTIONS
				if (reader.error())
				{
					err = std::move(reader.error());
					return;
				}
				#endif

				if (cp)
					parse_document();

				update_region_ends(root);
			}

			TOML_PUSH_WARNINGS
			TOML_DISABLE_INIT_WARNINGS

			[[nodiscard]]
			operator parse_result() && noexcept
			{
				#if TOML_EXCEPTIONS

				return { std::move(root) };

				#else

				if (err)
					return parse_result{ *std::move(err) };
				else
					return parse_result{ std::move(root) };

				#endif

			}

			TOML_POP_WARNINGS
	};

	inline std::unique_ptr<toml::array> parser::parse_array() TOML_MAY_THROW
	{
		TOML_ERROR_CHECK({});
		TOML_ASSERT(cp && *cp == U'[');

		const auto eof_check = [this]() TOML_MAY_THROW
		{
			TOML_ERROR_CHECK();
			if (!cp)
				abort_with_error("Encountered EOF while parsing array"sv);
		};

		// skip opening '['
		advance();
		eof_check();
		TOML_ERROR_CHECK({});

		auto arr = std::make_unique<array>();
		auto& vals = arr->values;

		enum parse_elem : int
		{
			none,
			comma,
			val
		};
		parse_elem prev = none;

		while (true)
		{
			TOML_ERROR_CHECK({});

			while (consume_leading_whitespace()
				|| consume_line_break()
				|| consume_comment())
				continue;
			eof_check();

			// commas - only legal after a value
			if (*cp == U',')
			{
				if (prev == val)
				{
					prev = comma;
					advance();
					continue;
				}
				abort_with_error(
					"Encountered unexpected character while parsing array; "
					"expected value or closing ']', saw comma"sv
				);
			}

			// closing ']'
			else if (*cp == U']')
			{
				advance();
				arr->source_.end = current_position_or_assumed_next();
				break;
			}

			// must be a value
			else
			{
				if (prev == val)
				{
					abort_with_error(
						"Encountered unexpected character while parsing array; "
						"expected comma or closing ']', saw '"sv, *cp, '\''
					);
					continue;
				}
				prev = val;

				vals.push_back(parse_value());
			}
		}

		TOML_ERROR_CHECK({});
		return arr;
	}

	inline std::unique_ptr<toml::table> parser::parse_inline_table() TOML_MAY_THROW
	{
		TOML_ERROR_CHECK({});
		TOML_ASSERT(cp && *cp == U'{');

		const auto eof_check = [this]() TOML_MAY_THROW
		{
			TOML_ERROR_CHECK();
			if (!cp)
				abort_with_error("Encountered EOF while parsing inline table"sv);
		};

		// skip opening '{'
		advance();
		eof_check();
		TOML_ERROR_CHECK({});

		auto tab = std::make_unique<table>();
		tab->inline_ = true;

		enum parse_elem : int
		{
			none,
			comma,
			kvp
		};
		parse_elem prev = none;

		while (true)
		{
			TOML_ERROR_CHECK({});

			if constexpr (TOML_LANG_HIGHER_THAN(0, 5, 0)) // toml/issues/516
			{
				while (consume_leading_whitespace()
					|| consume_line_break()
					|| consume_comment())
					continue;
			}
			else
			{
				while (consume_leading_whitespace())
					continue;
			}

			eof_check();

			// commas - only legal after a key-value pair
			if (*cp == U',')
			{
				if (prev == kvp)
				{
					prev = comma;
					advance();
				}
				else
					abort_with_error(
						"Encountered unexpected character while parsing inline table; "
						"expected key-value pair or closing '}', saw comma"sv
					);
			}

			// closing '}'
			else if (*cp == U'}')
			{
				if constexpr (!TOML_LANG_HIGHER_THAN(0, 5, 0)) // toml/issues/516
				{
					if (prev == comma)
					{
						abort_with_error(
							"Encountered unexpected character while parsing inline table; "
							"expected key-value pair, saw closing '}' (dangling comma)"sv
						);
						continue;
					}
				}

				advance();
				tab->source_.end = current_position_or_assumed_next();
				break;
			}

			// key-value pair
			#if TOML_LANG_HIGHER_THAN(0, 5, 0) // toml/issues/687
			else if (is_unicode_combining_mark(*cp))
			{
				abort_with_error(
					"Encountered unexpected character while parsing inline table; "
					"expected bare key starting character or string delimiter, saw combining mark '"sv, *cp, '\''
				);
			}
			#endif
			else if (is_string_delimiter(*cp) || is_bare_key_start_character(*cp))
			{
				if (prev == kvp)
					abort_with_error(
						"Encountered unexpected character while parsing inline table; "
						"expected comma or closing '}', saw '"sv, *cp, '\''
					);
				else
				{
					prev = kvp;
					parse_key_value_pair_and_insert(tab.get());
				}
			}

			/// ???
			else
				abort_with_error(
					"Encountered unexpected character while parsing inline table; "
					"expected key or closing '}', saw '"sv, *cp, '\''
				);
		}

		TOML_ERROR_CHECK({});
		return tab;
	}

	#undef TOML_ERROR_CHECK
	#undef TOML_ERROR
	#undef TOML_NORETURN
}

namespace toml
{
	[[nodiscard]]
	inline parse_result parse(std::string_view doc, std::string_view source_path = {}) TOML_MAY_THROW
	{
		return impl::parser{ impl::utf8_reader{ doc, source_path } };
	}

	[[nodiscard]]
	inline parse_result parse(std::string_view doc, std::string&& source_path) TOML_MAY_THROW
	{
		return impl::parser{ impl::utf8_reader{ doc, std::move(source_path) } };
	}

#if defined(__cpp_lib_char8_t)

	[[nodiscard]]
	inline parse_result parse(std::u8string_view doc, std::string_view source_path = {}) TOML_MAY_THROW
	{
		return impl::parser{ impl::utf8_reader{ doc, source_path } };
	}

	[[nodiscard]]
	inline parse_result parse(std::u8string_view doc, std::string&& source_path) TOML_MAY_THROW
	{
		return impl::parser{ impl::utf8_reader{ doc, std::move(source_path) } };
	}

#endif

	template <typename CHAR>
	[[nodiscard]]
	inline parse_result parse(std::basic_istream<CHAR>& doc, std::string_view source_path = {}) TOML_MAY_THROW
	{
		static_assert(
			sizeof(CHAR) == 1,
			"The stream's underlying character type must be 1 byte in size."
		);

		return impl::parser{ impl::utf8_reader{ doc, source_path } };
	}

	template <typename CHAR>
	[[nodiscard]]
	inline parse_result parse(std::basic_istream<CHAR>& doc, std::string&& source_path) TOML_MAY_THROW
	{
		static_assert(
			sizeof(CHAR) == 1,
			"The stream's underlying character type must be 1 byte in size."
		);

		return impl::parser{ impl::utf8_reader{ doc, std::move(source_path) } };
	}
}
