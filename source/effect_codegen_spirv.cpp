/**
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <assert.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
// Use the C++ variant of the SPIR-V headers
#include <spirv.hpp>
namespace spv {
#include <GLSL.std.450.h>
}

using namespace reshadefx;

/// <summary>
/// A single instruction in a SPIR-V module
/// </summary>
struct spirv_instruction
{
	// See: https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html
	// 0             | Opcode: The 16 high-order bits are the WordCount of the instruction. The 16 low-order bits are the opcode enumerant.
	// 1             | Optional instruction type <id>
	// .             | Optional instruction Result <id>
	// .             | Operand 1 (if needed)
	// .             | Operand 2 (if needed)
	// ...           | ...
	// WordCount - 1 | Operand N (N is determined by WordCount minus the 1 to 3 words used for the opcode, instruction type <id>, and instruction Result <id>).

	spv::Op op;
	spv::Id type;
	spv::Id result;
	std::vector<spv::Id> operands;

	explicit spirv_instruction(spv::Op op = spv::OpNop) : op(op), type(0), result(0) { }
	spirv_instruction(spv::Op op, spv::Id result) : op(op), type(result), result(0) { }
	spirv_instruction(spv::Op op, spv::Id type, spv::Id result) : op(op), type(type), result(result) { }

	/// <summary>
	/// Add a single operand to the instruction.
	/// </summary>
	spirv_instruction &add(spv::Id operand)
	{
		operands.push_back(operand);
		return *this;
	}

	/// <summary>
	/// Add a range of operands to the instruction.
	/// </summary>
	template <typename It>
	spirv_instruction &add(It begin, It end)
	{
		operands.insert(operands.end(), begin, end);
		return *this;
	}

	/// <summary>
	/// Add a null-terminated literal UTF-8 string to the instruction.
	/// </summary>
	spirv_instruction &add_string(const char *string)
	{
		uint32_t word;
		do {
			word = 0;
			for (uint32_t i = 0; i < 4 && *string; ++i)
				reinterpret_cast<uint8_t *>(&word)[i] = *string++;
			add(word);
		} while (*string || word & 0xFF000000);
		return *this;
	}
};

/// <summary>
/// A list of instructions forming a basic block in the SPIR-V module
/// </summary>
struct spirv_basic_block
{
	std::vector<spirv_instruction> instructions;

	/// <summary>
	/// Append another basic block the end of this one.
	/// </summary>
	void append(const spirv_basic_block &block)
	{
		instructions.insert(instructions.end(), block.instructions.begin(), block.instructions.end());
	}
};

static void write(std::vector<uint32_t> &s, uint32_t word)
{
	s.push_back(word);
}
static void write(std::vector<uint32_t> &s, const spirv_instruction &ins)
{
	// First word of an instruction:
	// The 16 low-order bits are the opcode
	// The 16 high-order bits are the word count of the instruction
	const uint32_t num_words = 1 + (ins.type != 0) + (ins.result != 0) + ins.operands.size();
	write(s, (num_words << spv::WordCountShift) | ins.op);

	// Optional instruction type ID
	if (ins.type != 0) write(s, ins.type);

	// Optional instruction result ID
	if (ins.result != 0) write(s, ins.result);

	// Write out the operands
	for (size_t i = 0; i < ins.operands.size(); ++i)
		write(s, ins.operands[i]);
}

static inline uint32_t align(uint32_t address, uint32_t alignment)
{
	return (address % alignment != 0) ? address + alignment - address % alignment : address;
};

class codegen_spirv final : public codegen
{
	struct function_blocks
	{
		spirv_basic_block declaration;
		spirv_basic_block variables;
		spirv_basic_block definition;
		type return_type;
		std::vector<type> param_types;

		friend bool operator==(const function_blocks &lhs, const function_blocks &rhs)
		{
			if (lhs.param_types.size() != rhs.param_types.size())
				return false;
			for (size_t i = 0; i < lhs.param_types.size(); ++i)
				if (!(lhs.param_types[i] == rhs.param_types[i]))
					return false;
			return lhs.return_type == rhs.return_type;
		}
	};

	spirv_basic_block _entries;
	spirv_basic_block _debug_a;
	spirv_basic_block _debug_b;
	spirv_basic_block _annotations;
	spirv_basic_block _types_and_constants;
	spirv_basic_block _variables;

	std::unordered_set<spv::Capability> _capabilities;
	std::vector<std::pair<type, spv::Id>> _type_lookup;
	std::vector<std::pair<function_blocks, spv::Id>> _function_type_lookup;
	std::vector<std::tuple<type, constant, spv::Id>> _constant_lookup;
	std::unordered_map<std::string, uint32_t> _semantic_to_location;
	std::unordered_map<std::string, spv::Id> _string_lookup;
	uint32_t _current_sampler_binding = 0;
	uint32_t _current_semantic_location = 10;

	std::vector<function_blocks> _functions2;
	std::unordered_map<id, spirv_basic_block> _block_data;
	spirv_basic_block *_current_block_data = nullptr;

	uint32_t _global_ubo_offset = 0;
	id _global_ubo_type = 0;
	id _global_ubo_variable = 0;

	id glsl_ext = 0;
	id _last_block = 0;

	void create_global_ubo()
	{
		struct_info global_ubo_type;
		global_ubo_type.definition = _global_ubo_type;
		for (const auto &uniform : uniforms)
			global_ubo_type.member_list.push_back({ uniform.type, uniform.name });

		define_struct({}, global_ubo_type);
		add_decoration(_global_ubo_type, spv::DecorationBlock);
		add_decoration(_global_ubo_type, spv::DecorationBinding, { 0 });
		add_decoration(_global_ubo_type, spv::DecorationDescriptorSet, { 0 });

		define_variable(_global_ubo_variable, {}, { type::t_struct, 0, 0, type::q_uniform, true, false, false, 0, _global_ubo_type }, "$Globals", spv::StorageClassUniform);
	}

	inline void add_location(const location &loc, spirv_basic_block &block)
	{
		if (loc.source.empty())
			return;

		spv::Id file = _string_lookup[loc.source];
		if (file == 0) {
			file = add_instruction(spv::OpString, 0, _debug_a)
				.add_string(loc.source.c_str())
				.result;
			_string_lookup[loc.source] = file;
		}

		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpLine
		add_instruction_without_result(spv::OpLine, block)
			.add(file)
			.add(loc.line)
			.add(loc.column);
	}
	inline spirv_instruction &add_instruction(spv::Op op, spv::Id type = 0)
	{
		assert(is_in_function() && is_in_block());
		return add_instruction(op, type, *_current_block_data);
	}
	inline spirv_instruction &add_instruction(spv::Op op, spv::Id type, spirv_basic_block &block)
	{
		spirv_instruction &instruction = add_instruction_without_result(op, block);
		instruction.type = type;
		instruction.result = make_id();
		return instruction;
	}
	inline spirv_instruction &add_instruction_without_result(spv::Op op)
	{
		assert(is_in_function() && is_in_block());
		return add_instruction_without_result(op, *_current_block_data);
	}
	inline spirv_instruction &add_instruction_without_result(spv::Op op, spirv_basic_block &block)
	{
		return block.instructions.emplace_back(op);
	}

	void write_result(module &s) const override
	{
		const_cast<codegen_spirv *>(this)->create_global_ubo();

		s.samplers = samplers;
		s.textures = textures;
		s.uniforms = uniforms;
		s.techniques = techniques;

		// Write SPIRV header info
		write(s.spirv, spv::MagicNumber);
		write(s.spirv, spv::Version);
		write(s.spirv, 0u); // Generator magic number, see https://www.khronos.org/registry/spir-v/api/spir-v.xml
		write(s.spirv, _next_id); // Maximum ID
		write(s.spirv, 0u); // Reserved for instruction schema

		// All capabilities
		write(s.spirv, spirv_instruction(spv::OpCapability)
			.add(spv::CapabilityMatrix));
		write(s.spirv, spirv_instruction(spv::OpCapability)
			.add(spv::CapabilityShader));

		for (spv::Capability capability : _capabilities)
			write(s.spirv, spirv_instruction(spv::OpCapability)
				.add(capability));

		write(s.spirv, spirv_instruction(spv::OpExtension)
			.add_string("SPV_GOOGLE_hlsl_functionality1"));

		// Optional extension instructions
		write(s.spirv, spirv_instruction(spv::OpExtInstImport, glsl_ext)
			.add_string("GLSL.std.450")); // Import GLSL extension

		// Single required memory model instruction
		write(s.spirv, spirv_instruction(spv::OpMemoryModel)
			.add(spv::AddressingModelLogical)
			.add(spv::MemoryModelGLSL450));

		// All entry point declarations
		for (const auto &node : _entries.instructions)
			write(s.spirv, node);

		// All debug instructions
		for (const auto &node : _debug_a.instructions)
			write(s.spirv, node);
		for (const auto &node : _debug_b.instructions)
			write(s.spirv, node);

		// All annotation instructions
		for (const auto &node : _annotations.instructions)
			write(s.spirv, node);

		// All type declarations
		for (const auto &node : _types_and_constants.instructions)
			write(s.spirv, node);
		for (const auto &node : _variables.instructions)
			write(s.spirv, node);

		// All function definitions
		for (const auto &function : _functions2)
		{
			if (function.definition.instructions.empty())
				continue;

			for (const auto &node : function.declaration.instructions)
				write(s.spirv, node);

			// Grab first label and move it in front of variable declarations
			write(s.spirv, function.definition.instructions.front());
			assert(function.definition.instructions.front().op == spv::OpLabel);

			for (const auto &node : function.variables.instructions)
				write(s.spirv, node);
			for (auto it = function.definition.instructions.begin() + 1; it != function.definition.instructions.end(); ++it)
				write(s.spirv, *it);
		}
	}

	spv::Id convert_type(const type &info)
	{
		if (auto it = std::find_if(_type_lookup.begin(), _type_lookup.end(),
			[&info](auto &x) { return x.first == info && (!info.is_ptr || (x.first.qualifiers & (type::q_static | type::q_uniform)) == (info.qualifiers & (type::q_static | type::q_uniform))); }); it != _type_lookup.end())
			return it->second;

		spv::Id type;

		if (info.is_ptr)
		{
			auto eleminfo = info;
			eleminfo.is_input = false;
			eleminfo.is_output = false;
			eleminfo.is_ptr = false;

			const spv::Id elemtype = convert_type(eleminfo);

			spv::StorageClass storage = spv::StorageClassFunction;
			if (info.is_input)
				storage = spv::StorageClassInput;
			if (info.is_output)
				storage = spv::StorageClassOutput;
			if (info.has(type::q_static))
				storage = spv::StorageClassPrivate;
			if (info.has(type::q_uniform))
				storage = (info.is_texture() || info.is_sampler()) ? spv::StorageClassUniformConstant : spv::StorageClassUniform;

			type = add_instruction(spv::OpTypePointer, 0, _types_and_constants)
				.add(storage)
				.add(elemtype)
				.result;
		}
		else if (info.is_array())
		{
			assert(!info.is_ptr);

			auto eleminfo = info;
			eleminfo.array_length = 0;

			const spv::Id elemtype = convert_type(eleminfo);

			if (info.array_length > 0) // Sized array
			{
				constant length_data = {};
				length_data.as_uint[0] = info.array_length;

				const spv::Id length_constant = emit_constant({ type::t_uint, 1, 1 }, length_data);

				type = add_instruction(spv::OpTypeArray, 0, _types_and_constants)
					.add(elemtype)
					.add(length_constant)
					.result;
			}
			else // Dynamic array
			{
				type = add_instruction(spv::OpTypeRuntimeArray, 0, _types_and_constants)
					.add(elemtype)
					.result;
			}
		}
		else if (info.is_matrix())
		{
			// Convert MxN matrix to a SPIR-V matrix with M vectors with N elements
			auto eleminfo = info;
			eleminfo.rows = info.cols;
			eleminfo.cols = 1;

			const spv::Id elemtype = convert_type(eleminfo);

			// Matrix types with just one row are interpreted as if they were a vector type
			if (info.rows == 1)
			{
				type = elemtype;
			}
			else
			{
				type = add_instruction(spv::OpTypeMatrix, 0, _types_and_constants)
					.add(elemtype)
					.add(info.rows)
					.result;
			}
		}
		else if (info.is_vector())
		{
			auto eleminfo = info;
			eleminfo.rows = 1;
			eleminfo.cols = 1;

			const spv::Id elemtype = convert_type(eleminfo);

			type = add_instruction(spv::OpTypeVector, 0, _types_and_constants)
				.add(elemtype)
				.add(info.rows)
				.result;
		}
		else
		{
			assert(!info.is_input && !info.is_output);

			switch (info.base)
			{
			case type::t_void:
				assert(info.rows == 0 && info.cols == 0);
				type = add_instruction(spv::OpTypeVoid, 0, _types_and_constants).result;
				break;
			case type::t_bool:
				assert(info.rows == 1 && info.cols == 1);
				type = add_instruction(spv::OpTypeBool, 0, _types_and_constants).result;
				break;
			case type::t_float:
				assert(info.rows == 1 && info.cols == 1);
				type = add_instruction( spv::OpTypeFloat, 0, _types_and_constants)
					.add(32)
					.result;
				break;
			case type::t_int:
				assert(info.rows == 1 && info.cols == 1);
				type = add_instruction(spv::OpTypeInt, 0, _types_and_constants)
					.add(32)
					.add(1)
					.result;
				break;
			case type::t_uint:
				assert(info.rows == 1 && info.cols == 1);
				type = add_instruction(spv::OpTypeInt, 0, _types_and_constants)
					.add(32)
					.add(0)
					.result;
				break;
			case type::t_struct:
				assert(info.rows == 0 && info.cols == 0 && info.definition != 0);
				type = info.definition;
				break;
			case type::t_texture: {
				assert(info.rows == 0 && info.cols == 0);
				spv::Id sampled_type = convert_type({ type::t_float, 1, 1 });
				type = add_instruction(spv::OpTypeImage, 0, _types_and_constants)
					.add(sampled_type) // Sampled Type
					.add(spv::Dim2D)
					.add(0) // Not a depth image
					.add(0) // Not an array
					.add(0) // Not multi-sampled
					.add(1) // Will be used with a sampler
					.add(spv::ImageFormatUnknown)
					.result;
				break;
			}
			case type::t_sampler: {
				assert(info.rows == 0 && info.cols == 0);
				spv::Id image_type = convert_type({ type::t_texture, 0, 0, type::q_uniform });
				type = add_instruction(spv::OpTypeSampledImage, 0, _types_and_constants)
					.add(image_type)
					.result;
				break;
			}
			default:
				assert(false);
				return 0;
			}
		}

		_type_lookup.push_back({ info, type });;

		return type;
	}
	spv::Id convert_type(const function_blocks &info)
	{
		if (auto it = std::find_if(_function_type_lookup.begin(), _function_type_lookup.end(), [&info](auto &x) { return x.first == info; }); it != _function_type_lookup.end())
			return it->second;

		spv::Id return_type = convert_type(info.return_type);
		assert(return_type != 0);
		std::vector<spv::Id> param_type_ids;
		for (auto param : info.param_types)
			param_type_ids.push_back(convert_type(param));

		spirv_instruction &node = add_instruction(spv::OpTypeFunction, 0, _types_and_constants);
		node.add(return_type);
		for (auto param_type : param_type_ids)
			node.add(param_type);

		_function_type_lookup.push_back({ info, node.result });;

		return node.result;
	}

	inline void add_name(id id, const char *name)
	{
		assert(name != nullptr);
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpName
		add_instruction_without_result(spv::OpName, _debug_b)
			.add(id)
			.add_string(name);
	}
	inline void add_builtin(id id, spv::BuiltIn builtin)
	{
		add_instruction_without_result(spv::OpDecorate, _annotations)
			.add(id)
			.add(spv::DecorationBuiltIn)
			.add(builtin);
	}
	inline void add_decoration(id id, spv::Decoration decoration, const char *string)
	{
		assert(string != nullptr);
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpDecorateStringGOOGLE
		add_instruction_without_result(spv::OpDecorateStringGOOGLE, _annotations)
			.add(id)
			.add(decoration)
			.add_string(string);
	}
	inline void add_decoration(id id, spv::Decoration decoration, std::initializer_list<uint32_t> values = {})
	{
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpDecorate
		add_instruction_without_result(spv::OpDecorate, _annotations)
			.add(id)
			.add(decoration)
			.add(values.begin(), values.end());
	}
	inline void add_member_name(id id, uint32_t member_index, const char *name)
	{
		assert(name != nullptr);
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpMemberName
		add_instruction_without_result(spv::OpMemberName, _debug_b)
			.add(id)
			.add(member_index)
			.add_string(name);
	}
	inline void add_member_builtin(id id, uint32_t member_index, spv::BuiltIn builtin)
	{
		add_instruction_without_result(spv::OpMemberDecorate, _annotations)
			.add(id)
			.add(member_index)
			.add(spv::DecorationBuiltIn)
			.add(builtin);
	}
	inline void add_member_decoration(id id, uint32_t member_index, spv::Decoration decoration, const char *string)
	{
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpMemberDecorateStringGOOGLE
		assert(string != nullptr);
		add_instruction_without_result(spv::OpMemberDecorateStringGOOGLE, _annotations)
			.add(id)
			.add(member_index)
			.add(decoration)
			.add_string(string);
	}
	inline void add_member_decoration(id id, uint32_t member_index, spv::Decoration decoration, std::initializer_list<uint32_t> values = {})
	{
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpMemberDecorate
		add_instruction_without_result(spv::OpMemberDecorate, _annotations)
			.add(id)
			.add(member_index)
			.add(decoration)
			.add(values.begin(), values.end());
	}
	inline void add_capability(spv::Capability capability)
	{
		_capabilities.insert(capability);
	}

	void define_variable(id id, const location &loc, const type &type, const char *name, spv::StorageClass storage, spv::Id initializer_value = 0)
	{
		assert(type.is_ptr);

		spirv_basic_block &block = storage != spv::StorageClassFunction ? _variables : _functions2[_current_function].variables;

		add_location(loc, block);

		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpVariable
		spirv_instruction &instruction = add_instruction_without_result(spv::OpVariable, block);
		instruction.type = convert_type(type);
		instruction.result = id;
		instruction.add(storage);
		if (initializer_value != 0)
			instruction.add(initializer_value);

		if (name != nullptr && *name != '\0')
			add_name(id, name);
	}

	id define_struct(const location &loc, struct_info &info) override
	{
		structs.push_back(info);

		add_location(loc, _types_and_constants);

		spirv_instruction &instruction = add_instruction_without_result(spv::OpTypeStruct, _types_and_constants);
		instruction.result = info.definition;

		for (const auto &member : info.member_list)
			instruction.add(convert_type(member.type));

		if (!info.unique_name.empty())
			add_name(info.definition, info.unique_name.c_str());

		for (uint32_t index = 0; index < info.member_list.size(); ++index)
			add_member_name(info.definition, index, info.member_list[index].name.c_str());

		return info.definition;
	}
	id define_texture(const location &, texture_info &info) override
	{
		textures.push_back(info);

		return info.id;
	}
	id define_sampler(const location &loc, sampler_info &info) override
	{
		info.set = 1;
		info.binding = _current_sampler_binding++;

		define_variable(info.id, loc, { type::t_sampler, 0, 0, type::q_extern | type::q_uniform, true }, info.unique_name.c_str(), spv::StorageClassUniformConstant);

		add_decoration(info.id, spv::DecorationBinding, { info.binding });
		add_decoration(info.id, spv::DecorationDescriptorSet, { info.set });

		samplers.push_back(info);

		return info.id;
	}
	id define_uniform(const location &, uniform_info &info) override
	{
		if (_global_ubo_type == 0)
			_global_ubo_type = make_id();
		if (_global_ubo_variable == 0)
			_global_ubo_variable = make_id();

		// GLSL specification on std140 layout:
		// 1. If the member is a scalar consuming N basic machine units, the base alignment is N.
		// 2. If the member is a two- or four-component vector with components consuming N basic machine units, the base alignment is 2N or 4N, respectively.
		// 3. If the member is a three-component vector with components consuming N basic machine units, the base alignment is 4N.
		unsigned int size = 4 * (info.type.rows == 3 ? 4 : info.type.rows) * info.type.cols * std::max(1, info.type.array_length);
		unsigned int alignment = size;
		info.offset = align(_global_ubo_offset, alignment);

		_global_ubo_offset = info.offset + size;

		info.member_index = uniforms.size();
		info.struct_type_id = _global_ubo_type;

		uniforms.push_back(info);

		add_member_decoration(_global_ubo_type, info.member_index, spv::DecorationOffset, { info.offset });

		return _global_ubo_variable;
	}
	id define_variable(const location &loc, const type &type, const char *name, bool global, id initializer_value) override
	{
		id id = make_id();
		define_variable(id, loc, type, name, global ? spv::StorageClassPrivate : spv::StorageClassFunction, initializer_value);

		return id;
	}
	id define_function(const location &, function_info &info) override
	{
		functions.push_back(std::make_unique<function_info>(info));

		if (!info.name.empty())
			add_name(info.definition, info.name.c_str());

		return info.definition;
	}
	id define_parameter(const location &loc, struct_member_info &info) override
	{
		id id = make_id();

		_functions2[_current_function].param_types.push_back(info.type);

		add_location(loc, _functions2[_current_function].declaration);

		spirv_instruction &instruction = add_instruction_without_result(spv::OpFunctionParameter, _functions2[_current_function].declaration);
		instruction.type = convert_type(info.type);
		instruction.result = id;

		add_name(id, info.name.c_str());

		return id;
	}
	id define_technique(const location &, technique_info &info) override
	{
		techniques.push_back(info);

		return 0;
	}

	id create_entry_point(const function_info &func, bool is_ps) override
	{
		std::vector<expression> call_params;
		std::vector<unsigned int> inputs_and_outputs;

		// Generate the glue entry point function
		function_info entry_point;
		entry_point.definition = make_id();
		entry_point.entry_block = make_id();
		entry_point.return_type = { type::t_void };

		enter_function(entry_point.definition, entry_point.return_type);
		enter_block(entry_point.entry_block);

		const auto semantic_to_builtin = [is_ps](const std::string &semantic, spv::BuiltIn &builtin) {
			builtin = spv::BuiltInMax;
			if (semantic == "SV_POSITION")
				builtin = is_ps ? spv::BuiltInFragCoord : spv::BuiltInPosition;
			if (semantic == "SV_POINTSIZE")
				builtin = spv::BuiltInPointSize;
			if (semantic == "SV_DEPTH")
				builtin = spv::BuiltInFragDepth;
			if (semantic == "VERTEXID" || semantic == "SV_VERTEXID")
				builtin = spv::BuiltInVertexId;
			return builtin != spv::BuiltInMax;
		};

		const auto create_input_param = [this, &call_params](const struct_member_info &param) {
			const auto function_variable = make_id();
			define_variable(function_variable, {}, param.type, nullptr, spv::StorageClassFunction);
			call_params.emplace_back().reset_to_lvalue({}, function_variable, param.type);
			return function_variable;
		};
		const auto create_input_variable = [this, &inputs_and_outputs, &semantic_to_builtin](const struct_member_info &param) {
			type input_type = param.type;
			input_type.is_input = true;
			input_type.is_ptr = true;

			const auto input_variable = make_id();
			define_variable(input_variable, {}, input_type, nullptr, spv::StorageClassInput);

			if (spv::BuiltIn builtin; semantic_to_builtin(param.semantic, builtin))
				add_builtin(input_variable, builtin);
			else
			{
				uint32_t location = 0;
				if (param.semantic.size() >= 5 && param.semantic.compare(0, 5, "COLOR") == 0)
					location = std::strtol(param.semantic.substr(5).c_str(), nullptr, 10);
				else if (param.semantic.size() >= 9 && param.semantic.compare(0, 9, "SV_TARGET") == 0)
					location = std::strtol(param.semantic.substr(9).c_str(), nullptr, 10);
				else if (param.semantic.size() >= 8 && param.semantic.compare(0, 8, "TEXCOORD") == 0)
					location = std::strtol(param.semantic.substr(8).c_str(), nullptr, 10);
				else if (const auto it = _semantic_to_location.find(param.semantic); it != _semantic_to_location.end())
					location = it->second;
				else
					_semantic_to_location[param.semantic] = location = _current_semantic_location++;

				add_decoration(input_variable, spv::DecorationLocation, { location });
			}

			if (param.type.has(type::q_noperspective))
				add_decoration(input_variable, spv::DecorationNoPerspective);
			if (param.type.has(type::q_centroid))
				add_decoration(input_variable, spv::DecorationCentroid);
			if (param.type.has(type::q_nointerpolation))
				add_decoration(input_variable, spv::DecorationFlat);

			inputs_and_outputs.push_back(input_variable);
			return input_variable;
		};
		const auto create_output_param = [this, &call_params](const struct_member_info &param) {
			const auto function_variable = make_id();
			define_variable(function_variable, {}, param.type, nullptr, spv::StorageClassFunction);
			call_params.emplace_back().reset_to_lvalue({}, function_variable, param.type);
			return function_variable;
		};
		const auto create_output_variable = [this, &inputs_and_outputs, &semantic_to_builtin](const struct_member_info &param) {
			type output_type = param.type;
			output_type.is_output = true;
			output_type.is_ptr = true;

			const auto output_variable = make_id();
			define_variable(output_variable, {}, output_type, nullptr, spv::StorageClassOutput);

			if (spv::BuiltIn builtin; semantic_to_builtin(param.semantic, builtin))
				add_builtin(output_variable, builtin);
			else
			{
				uint32_t location = 0;
				if (param.semantic.size() >= 5 && param.semantic.compare(0, 5, "COLOR") == 0)
					location = std::strtol(param.semantic.substr(5).c_str(), nullptr, 10);
				else if (param.semantic.size() >= 9 && param.semantic.compare(0, 9, "SV_TARGET") == 0)
					location = std::strtol(param.semantic.substr(9).c_str(), nullptr, 10);
				else if (param.semantic.size() >= 8 && param.semantic.compare(0, 8, "TEXCOORD") == 0)
					location = std::strtol(param.semantic.substr(8).c_str(), nullptr, 10);
				else if (const auto it = _semantic_to_location.find(param.semantic); it != _semantic_to_location.end())
					location = it->second;
				else
					_semantic_to_location[param.semantic] = location = _current_semantic_location++;

				add_decoration(output_variable, spv::DecorationLocation, { location });
			}

			if (param.type.has(type::q_noperspective))
				add_decoration(output_variable, spv::DecorationNoPerspective);
			if (param.type.has(type::q_centroid))
				add_decoration(output_variable, spv::DecorationCentroid);
			if (param.type.has(type::q_nointerpolation))
				add_decoration(output_variable, spv::DecorationFlat);

			inputs_and_outputs.push_back(output_variable);
			return output_variable;
		};

		// Handle input parameters
		for (const struct_member_info &param : func.parameter_list)
		{
			if (param.type.has(type::q_out))
			{
				create_output_param(param);

				// Flatten structure parameters
				if (param.type.is_struct())
				{
					for (const auto &member : find_struct(param.type.definition).member_list)
					{
						create_output_variable(member);
					}
				}
				else
				{
					create_output_variable(param);
				}
			}
			else
			{
				const auto param_variable = create_input_param(param);

				// Flatten structure parameters
				if (param.type.is_struct())
				{
					std::vector<unsigned int> elements;

					for (const auto &member : find_struct(param.type.definition).member_list)
					{
						const auto input_variable = create_input_variable(member);

						type value_type = member.type;
						value_type.is_ptr = false;

						const auto value = add_instruction(spv::OpLoad, convert_type(value_type))
							.add(input_variable)
							.result;
						elements.push_back(value);
					}

					type composite_type = param.type;
					composite_type.is_ptr = false;
					spirv_instruction &construct = add_instruction(spv::OpCompositeConstruct, convert_type(composite_type));
					for (auto elem : elements)
						construct.add(elem);
					const auto composite_value = construct.result;

					add_instruction_without_result(spv::OpStore)
						.add(param_variable)
						.add(composite_value);
				}
				else
				{
					const auto input_variable = create_input_variable(param);

					type value_type = param.type;
					value_type.is_ptr = false;

					const auto value = add_instruction(spv::OpLoad, convert_type(value_type))
						.add(input_variable)
						.result;
					add_instruction_without_result(spv::OpStore)
						.add(param_variable)
						.add(value);
				}
			}
		}

		const auto call_result = emit_call({}, func.definition, func.return_type, call_params);

		size_t param_index = 0;
		size_t inputs_and_outputs_index = 0;
		for (const struct_member_info &param : func.parameter_list)
		{
			if (param.type.has(type::q_out))
			{
				type value_type = param.type;
				value_type.is_ptr = false;

				const auto value = add_instruction(spv::OpLoad, convert_type(value_type))
					.add(call_params[param_index++].base)
					.result;

				if (param.type.is_struct())
				{
					unsigned int member_index = 0;
					for (const auto &member : find_struct(param.type.definition).member_list)
					{
						const auto member_value = add_instruction(spv::OpCompositeExtract, convert_type(member.type))
							.add(value)
							.add(member_index++)
							.result;
						add_instruction_without_result(spv::OpStore)
							.add(inputs_and_outputs[inputs_and_outputs_index++])
							.add(member_value);
					}
				}
				else
				{
					add_instruction_without_result(spv::OpStore)
						.add(inputs_and_outputs[inputs_and_outputs_index++])
						.add(value);
				}
			}
			else
			{
				param_index++;
				inputs_and_outputs_index += param.type.is_struct() ? find_struct(param.type.definition).member_list.size() : 1;
			}
		}

		if (func.return_type.is_struct())
		{
			unsigned int member_index = 0;
			for (const auto &member : find_struct(func.return_type.definition).member_list)
			{
				const auto result = create_output_variable(member);

				const auto member_result = add_instruction(spv::OpCompositeExtract, convert_type(member.type))
					.add(call_result)
					.add(member_index)
					.result;

				add_instruction_without_result(spv::OpStore)
					.add(result)
					.add(member_result);

				member_index++;
			}
		}
		else if (!func.return_type.is_void())
		{
			type ptr_type = func.return_type;
			ptr_type.is_output = true;
			ptr_type.is_ptr = true;

			const auto result = make_id();
			define_variable(result, {}, ptr_type, nullptr, spv::StorageClassOutput);

			if (spv::BuiltIn builtin; semantic_to_builtin(func.return_semantic, builtin))
				add_builtin(result, builtin);
			else
			{
				uint32_t semantic_location = 0;
				if (func.return_semantic.size() >= 5 && func.return_semantic.compare(0, 5, "COLOR") == 0)
					semantic_location = std::strtol(func.return_semantic.substr(5).c_str(), nullptr, 10);
				else if (func.return_semantic.size() >= 9 && func.return_semantic.compare(0, 9, "SV_TARGET") == 0)
					semantic_location = std::strtol(func.return_semantic.substr(9).c_str(), nullptr, 10);
				else if (func.return_semantic.size() >= 8 && func.return_semantic.compare(0, 8, "TEXCOORD") == 0)
					semantic_location = std::strtol(func.return_semantic.substr(8).c_str(), nullptr, 10);
				else if (const auto it = _semantic_to_location.find(func.return_semantic); it != _semantic_to_location.end())
					semantic_location = it->second;
				else
					_semantic_to_location[func.return_semantic] = semantic_location = _current_semantic_location++;

				add_decoration(result, spv::DecorationLocation, { semantic_location });
			}

			inputs_and_outputs.push_back(result);

			add_instruction_without_result(spv::OpStore)
				.add(result)
				.add(call_result);
		}

		leave_block_and_return(0);
		define_function({}, entry_point);
		leave_function();

		assert(!func.name.empty());
		add_instruction_without_result(spv::OpEntryPoint, _entries)
			.add(is_ps ? spv::ExecutionModelFragment : spv::ExecutionModelVertex)
			.add(entry_point.definition)
			.add_string(func.name.c_str())
			.add(inputs_and_outputs.begin(), inputs_and_outputs.end());

		return entry_point.definition;
	}

	id emit_constant(const type &type, const constant &data) override
	{
		assert(!type.is_ptr);

		if (auto it = std::find_if(_constant_lookup.begin(), _constant_lookup.end(), [&type, &data](auto &x) {
			if (!(std::get<0>(x) == type && std::memcmp(&std::get<1>(x).as_uint[0], &data.as_uint[0], sizeof(uint32_t) * 16) == 0 && std::get<1>(x).array_data.size() == data.array_data.size()))
				return false;
			for (size_t i = 0; i < data.array_data.size(); ++i)
				if (std::memcmp(&std::get<1>(x).array_data[i].as_uint[0], &data.array_data[i].as_uint[0], sizeof(uint32_t) * 16) != 0)
					return false;
			return true;
		}); it != _constant_lookup.end())
			return std::get<2>(*it);

		spv::Id result = 0;

		if (type.is_array())
		{
			assert(type.array_length > 0);

			std::vector<spv::Id> elements;

			auto elem_type = type;
			elem_type.array_length = 0;

			for (const constant &elem : data.array_data)
				elements.push_back(emit_constant(elem_type, elem));
			for (size_t i = elements.size(); i < static_cast<size_t>(type.array_length); ++i)
				elements.push_back(emit_constant(elem_type, {}));

			spirv_instruction &node = add_instruction(spv::OpConstantComposite, convert_type(type), _types_and_constants);

			for (spv::Id elem : elements)
				node.add(elem);

			result = node.result;
		}
		else if (type.is_struct())
		{
			result = add_instruction(spv::OpConstantNull, convert_type(type), _types_and_constants).result;
		}
		else if (type.is_matrix())
		{
			spv::Id rows[4] = {};

			for (unsigned int i = 0; i < type.rows; ++i)
			{
				auto row_type = type;
				row_type.rows = type.cols;
				row_type.cols = 1;
				constant row_data = {};
				for (unsigned int k = 0; k < type.cols; ++k)
					row_data.as_uint[k] = data.as_uint[i * type.cols + k];

				rows[i] = emit_constant(row_type, row_data);
			}

			if (type.rows == 1)
			{
				result = rows[0];
			}
			else
			{
				spirv_instruction &node = add_instruction(spv::OpConstantComposite, convert_type(type), _types_and_constants);

				for (unsigned int i = 0; i < type.rows; ++i)
					node.add(rows[i]);

				result = node.result;
			}
		}
		else if (type.is_vector())
		{
			spv::Id rows[4] = {};

			for (unsigned int i = 0; i < type.rows; ++i)
			{
				auto scalar_type = type;
				scalar_type.rows = 1;
				constant scalar_data = {};
				scalar_data.as_uint[0] = data.as_uint[i];

				rows[i] = emit_constant(scalar_type, scalar_data);
			}

			spirv_instruction &node = add_instruction(spv::OpConstantComposite, convert_type(type), _types_and_constants);

			for (unsigned int i = 0; i < type.rows; ++i)
				node.add(rows[i]);

			result = node.result;
		}
		else if (type.is_boolean())
		{
			result = add_instruction(data.as_uint[0] ? spv::OpConstantTrue : spv::OpConstantFalse, convert_type(type), _types_and_constants).result;
		}
		else
		{
			assert(type.is_scalar());
			result = add_instruction(spv::OpConstant, convert_type(type), _types_and_constants).add(data.as_uint[0]).result;
		}

		_constant_lookup.push_back({ type, data, result });

		return result;
	}

	id emit_unary_op(const location &loc, tokenid op, const type &type, id val) override
	{
		spv::Op spv_op = spv::OpNop;

		switch (op)
		{
		case tokenid::exclaim: spv_op = spv::OpLogicalNot; break;
		case tokenid::minus: spv_op = type.is_floating_point() ? spv::OpFNegate : spv::OpSNegate; break;
		case tokenid::tilde: spv_op = spv::OpNot; break;
		case tokenid::plus_plus: spv_op = type.is_floating_point() ? spv::OpFAdd : spv::OpIAdd; break;
		case tokenid::minus_minus: spv_op = type.is_floating_point() ? spv::OpFSub : spv::OpISub; break;
		default:
			return assert(false), 0;
		}

		add_location(loc, *_current_block_data);

		const spv::Id result = add_instruction(spv_op, convert_type(type))
			.add(val) // Operand
			.result; // Result ID

		return result;
	}
	id emit_binary_op(const location &loc, tokenid op, const type &res_type, const type &type, id lhs, id rhs) override
	{
		spv::Op spv_op = spv::OpNop;

		switch (op)
		{
		case tokenid::percent:
		case tokenid::percent_equal: spv_op = type.is_floating_point() ? spv::OpFRem : type.is_signed() ? spv::OpSRem : spv::OpUMod; break;
		case tokenid::ampersand:
		case tokenid::ampersand_equal: spv_op = spv::OpBitwiseAnd; break;
		case tokenid::star:
		case tokenid::star_equal: spv_op = type.is_floating_point() ? spv::OpFMul : spv::OpIMul; break;
		case tokenid::plus:
		case tokenid::plus_plus:
		case tokenid::plus_equal: spv_op = type.is_floating_point() ? spv::OpFAdd : spv::OpIAdd; break;
		case tokenid::minus:
		case tokenid::minus_minus:
		case tokenid::minus_equal: spv_op = type.is_floating_point() ? spv::OpFSub : spv::OpISub; break;
		case tokenid::slash:
		case tokenid::slash_equal: spv_op = type.is_floating_point() ? spv::OpFDiv : type.is_signed() ? spv::OpSDiv : spv::OpUDiv; break;
		case tokenid::less: spv_op = type.is_floating_point() ? spv::OpFOrdLessThan : type.is_signed() ? spv::OpSLessThan : spv::OpULessThan; break;
		case tokenid::greater: spv_op = type.is_floating_point() ? spv::OpFOrdGreaterThan : type.is_signed() ? spv::OpSGreaterThan : spv::OpUGreaterThan; break;
		case tokenid::caret:
		case tokenid::caret_equal: spv_op = spv::OpBitwiseXor; break;
		case tokenid::pipe:
		case tokenid::pipe_equal: spv_op = spv::OpBitwiseOr; break;
		case tokenid::exclaim_equal: spv_op = type.is_integral() ? spv::OpINotEqual : type.is_floating_point() ? spv::OpFOrdNotEqual : spv::OpLogicalNotEqual; break;
		case tokenid::ampersand_ampersand: spv_op = spv::OpLogicalAnd;  break;
		case tokenid::less_less:
		case tokenid::less_less_equal: spv_op = spv::OpShiftLeftLogical; break;
		case tokenid::less_equal: spv_op = type.is_floating_point() ? spv::OpFOrdLessThanEqual : type.is_signed() ? spv::OpSLessThanEqual : spv::OpULessThanEqual; break;
		case tokenid::equal_equal: spv_op = type.is_floating_point() ? spv::OpFOrdEqual : type.is_integral() ? spv::OpIEqual : spv::OpLogicalEqual; break;
		case tokenid::greater_greater:
		case tokenid::greater_greater_equal: spv_op = type.is_signed() ? spv::OpShiftRightArithmetic : spv::OpShiftRightLogical; break;
		case tokenid::greater_equal: spv_op = type.is_floating_point() ? spv::OpFOrdGreaterThanEqual : type.is_signed() ? spv::OpSGreaterThanEqual : spv::OpUGreaterThanEqual; break;
		case tokenid::pipe_pipe: spv_op = spv::OpLogicalOr; break;
		default:
			return assert(false), 0;
		}

		add_location(loc, *_current_block_data);

		const spv::Id result = add_instruction(spv_op, convert_type(res_type))
			.add(lhs) // Operand 1
			.add(rhs) // Operand 2
			.result; // Result ID

		if (res_type.has(type::q_precise))
			add_decoration(result, spv::DecorationNoContraction);

		return result;
	}
	id emit_ternary_op(const location &loc, tokenid op, const type &type, id condition, id true_value, id false_value) override
	{
		assert(op == tokenid::question);

		add_location(loc, *_current_block_data);

		const spv::Id result = add_instruction(spv::OpSelect, convert_type(type))
			.add(condition) // Condition
			.add(true_value) // Object 1
			.add(false_value) // Object 2
			.result; // Result ID

		return result;
	}
	id emit_phi(const type &type, id lhs_value, id lhs_block, id rhs_value, id rhs_block) override
	{
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpPhi
		const spv::Id result = add_instruction(spv::OpPhi, convert_type(type))
			.add(lhs_value) // Variable 0
			.add(lhs_block) // Parent 0
			.add(rhs_value) // Variable 1
			.add(rhs_block) // Parent 1
			.result;

		return result;
	}
	id emit_call(const location &loc, id function, const type &res_type, const std::vector<expression> &args) override
	{
		add_location(loc, *_current_block_data);

		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpFunctionCall
		spirv_instruction &call = add_instruction(spv::OpFunctionCall, convert_type(res_type))
			.add(function); // Function
		for (size_t i = 0; i < args.size(); ++i)
			call.add(args[i].base); // Arguments

		return call.result;
	}
	id emit_call_intrinsic(const location &loc, id intrinsic, const type &res_type, const std::vector<expression> &args) override
	{
		add_location(loc, *_current_block_data);

		enum
		{
#define IMPLEMENT_INTRINSIC_SPIRV(name, i, code) name##i,
#include "effect_symbol_table_intrinsics.inl"
		};

		switch (intrinsic)
		{
#define IMPLEMENT_INTRINSIC_SPIRV(name, i, code) case name##i: code
#include "effect_symbol_table_intrinsics.inl"
		default:
			return 0;
		}
	}
	id emit_construct(const type &type, std::vector<expression> &args) override
	{
		std::vector<spv::Id> ids;

		// There must be exactly one constituent for each top-level component of the result
		if (type.is_matrix())
		{
			assert(type.rows == type.cols);

			// First, extract all arguments so that a list of scalars exist
			for (auto &argument : args)
			{
				if (!argument.type.is_scalar())
				{
					for (unsigned int index = 0; index < argument.type.components(); ++index)
					{
						expression scalar = argument;
						scalar.add_static_index_access(this, index);
						auto scalar_type = scalar.type;
						scalar_type.base = type.base;
						scalar.add_cast_operation(scalar_type);
						ids.push_back(emit_load(scalar));
						assert(ids.back() != 0);
					}
				}
				else
				{
					auto scalar_type = argument.type;
					scalar_type.base = type.base;
					argument.add_cast_operation(scalar_type);
					ids.push_back(emit_load(argument));
					assert(ids.back() != 0);
				}
			}

			// Second, turn that list of scalars into a list of column vectors
			for (size_t i = 0, j = 0; i < ids.size(); i += type.rows, ++j)
			{
				auto vector_type = type;
				vector_type.cols = 1;

				spirv_instruction &node = add_instruction(spv::OpCompositeConstruct, convert_type(vector_type));
				for (unsigned int k = 0; k < type.rows; ++k)
					node.add(ids[i + k]);

				ids[j] = node.result;
			}

			ids.erase(ids.begin() + type.cols, ids.end());

			// Finally, construct a matrix from those column vectors
			spirv_instruction &node = add_instruction(spv::OpCompositeConstruct, convert_type(type));

			for (size_t i = 0; i < ids.size(); i += type.rows)
			{
				node.add(ids[i]);
			}

		}
		// The exception is that for constructing a vector, a contiguous subset of the scalars consumed can be represented by a vector operand instead
		else
		{
			assert(type.is_vector() || type.is_array());

			for (expression &argument : args)
			{
				auto target_type = argument.type;
				target_type.base = type.base;
				argument.add_cast_operation(target_type);
				assert(argument.type.is_scalar() || argument.type.is_vector());

				ids.push_back(emit_load(argument));
				assert(ids.back() != 0);
			}
		}

		return add_instruction(spv::OpCompositeConstruct, convert_type(type))
			.add(ids.begin(), ids.end())
			.result;
	}

	void emit_if(const location &loc, id, id prev_block, id true_statement_block, id false_statement_block, id merge_label, unsigned int flags) override
	{
		int selection_control = 0;
		if (flags & flatten) selection_control |= spv::SelectionControlFlattenMask;
		if (flags & dont_flatten) selection_control |= spv::SelectionControlDontFlattenMask;

		//_block_data[merge_label].append(_block_data[prev_block]);

		//add_location(loc, _block_data[merge_label]);

		//add_instruction_without_result(spv::OpSelectionMerge, _block_data[merge_label])
		//	.add(merge_label) // Merge Block
		//	.add(selection_control); // Selection Control

		//_block_data[merge_label].append(_block_data[true_statement_block]);
		//_block_data[merge_label].append(_block_data[false_statement_block]);

		add_instruction_without_result(spv::OpSelectionMerge)
			.add(merge_label) // Merge Block
			.add(selection_control); // Selection Control
	}
	void emit_loop(const location &loc, id, id prev_block, id header_label, id condition_label, id loop_label, id continue_label, id merge_label, unsigned int flags) override
	{
		int loop_control = 0;
		if (flags & unroll) loop_control |= spv::LoopControlUnrollMask;
		if (flags & dont_unroll) loop_control |= spv::LoopControlDontUnrollMask;

		//assert(_block_data[header_label].instructions.size() == 2);

		//_block_data[prev_block].instructions.push_back(_block_data[header_label].instructions[0]);

		//add_location(loc, _block_data[prev_block]);

		//add_instruction_without_result(spv::OpLoopMerge, _block_data[prev_block])
		//	.add(merge_label) // Merge Block
		//	.add(continue_label) // Continue Target
		//	.add(loop_control); // Loop Control

		//_block_data[prev_block].instructions.push_back(_block_data[header_label].instructions[1]);
		//if (condition_label)
		//	_block_data[prev_block].append(_block_data[condition_label]);
		//_block_data[prev_block].append(_block_data[loop_label]);
		//_block_data[prev_block].append(_block_data[continue_label]);

		//_block_data[merge_label].append(_block_data[prev_block]);

		add_instruction_without_result(spv::OpLoopMerge)
			.add(merge_label) // Merge Block
			.add(continue_label) // Continue Target
			.add(loop_control); // Loop Control
	}
	void emit_switch(const location &loc, id, id prev_block, id default_label, const std::vector<id> &case_literal_and_labels, id merge_label, unsigned int flags) override
	{
		//int selection_control = 0;
		//if (flags & flatten) selection_control |= spv::SelectionControlFlattenMask;
		//if (flags & dont_flatten) selection_control |= spv::SelectionControlDontFlattenMask;

		//add_location(loc, _block_data[merge_label]);

		//add_instruction_without_result(spv::OpSelectionMerge, _block_data[merge_label])
		//	.add(merge_label) // Merge Block
		//	.add(selection_control); // Selection Control

		//assert(is_in_function() && !is_in_block());
		//spirv_instruction &switch_instruction = _block_data[prev_block].instructions.back();
		//assert(switch_instruction.op == spv::OpSwitch);
		//switch_instruction.add(default_label);
		//switch_instruction.add(case_literal_and_labels.begin(), case_literal_and_labels.end());

		//_block_data[merge_label].append(_block_data[prev_block]);

		//for (size_t i = 0; i < case_literal_and_labels.size(); i += 2)
		//{
		//	_block_data[merge_label].append(_block_data[case_literal_and_labels[i + 1]]);
		//}

		//_block_data[merge_label].append(_block_data[default_label]);
	}

	  id emit_load(const expression &chain) override
	{
		add_location(chain.location, *_current_block_data);

		if (chain.is_constant) // Constant expressions do not have a complex access chain
			return emit_constant(chain.type, chain.constant);

		spv::Id result = chain.base;

		size_t op_index2 = 0;

		// If a variable is referenced, load the value first
		if (chain.is_lvalue)
		{
			auto base_type = chain.type;
			if (!chain.ops.empty())
				base_type = chain.ops[0].from;

			// Any indexing expressions can be resolved during load with an 'OpAccessChain' already
			if (!chain.ops.empty() && chain.ops[0].type == expression::operation::op_index)
			{
				assert(chain.ops[0].to.is_ptr);
				spirv_instruction &node = add_instruction(spv::OpAccessChain)
					.add(result); // Base

				// Ignore first index into 1xN matrices, since they were translated to a vector type in SPIR-V
				if (chain.ops[0].from.rows == 1 && chain.ops[0].from.cols > 1)
					op_index2 = 1;

				do {
					assert(chain.ops[op_index2].to.is_ptr);
					base_type = chain.ops[op_index2].to;
					node.add(chain.ops[op_index2++].index); // Indexes
				} while (op_index2 < chain.ops.size() && chain.ops[op_index2].type == expression::operation::op_index);
				node.type = convert_type(chain.ops[op_index2 - 1].to); // Last type is the result
				result = node.result; // Result ID
			}

			base_type.is_ptr = false;

			result = add_instruction(spv::OpLoad, convert_type(base_type))
				.add(result) // Pointer
				.result; // Result ID
		}

		// Work through all remaining operations in the access chain and apply them to the value
		for (; op_index2 < chain.ops.size(); ++op_index2)
		{
			const auto &op = chain.ops[op_index2];

			switch (op.type)
			{
			case expression::operation::op_cast:
				assert(!op.to.is_ptr);

				if (op.from.base != op.to.base)
				{
					type from_with_to_base = op.from;
					from_with_to_base.base = op.to.base;

					if (op.from.is_boolean())
					{
						constant true_value = {};
						constant false_value = {};
						for (unsigned int i = 0; i < op.to.components(); ++i)
							true_value.as_uint[i] = op.to.is_floating_point() ? 0x3f800000 : 1;
						const spv::Id true_constant = emit_constant(from_with_to_base, true_value);
						const spv::Id false_constant = emit_constant(from_with_to_base, false_value);

						result = add_instruction(spv::OpSelect, convert_type(from_with_to_base))
							.add(result) // Condition
							.add(true_constant)
							.add(false_constant)
							.result;
					}
					else
					{
						switch (op.to.base)
						{
						case type::t_bool:
							result = add_instruction(op.from.is_floating_point() ? spv::OpFOrdNotEqual : spv::OpINotEqual, convert_type(from_with_to_base))
								.add(result)
								.add(emit_constant(op.from, {}))
								.result;
							break;
						case type::t_int:
							result = add_instruction(op.from.is_floating_point() ? spv::OpConvertFToS : spv::OpBitcast, convert_type(from_with_to_base))
								.add(result)
								.result;
							break;
						case type::t_uint:
							result = add_instruction(op.from.is_floating_point() ? spv::OpConvertFToU : spv::OpBitcast, convert_type(from_with_to_base))
								.add(result)
								.result;
							break;
						case type::t_float:
							assert(op.from.is_integral());
							result = add_instruction(op.from.is_signed() ? spv::OpConvertSToF : spv::OpConvertUToF, convert_type(from_with_to_base))
								.add(result)
								.result;
							break;
						}
					}
				}

				if (op.to.components() > op.from.components())
				{
					spirv_instruction &composite_node = add_instruction(chain.is_constant ? spv::OpConstantComposite : spv::OpCompositeConstruct, convert_type(op.to));
					for (unsigned int i = 0; i < op.to.components(); ++i)
						composite_node.add(result);
					result = composite_node.result;
				}
				if (op.from.components() > op.to.components())
				{
					//signed char swizzle[4] = { -1, -1, -1, -1 };
					//for (unsigned int i = 0; i < rhs.type.rows; ++i)
					//	swizzle[i] = i;
					//from.push_swizzle(swizzle);
					assert(false); // TODO
				}
				break;
			case expression::operation::op_index:
				if (op.from.is_array())
				{
					assert(false);
					/*assert(result != 0);
					//result = add_instruction(section, chain.location, spv::OpCompositeExtract, convert_type(op.to))
					//	.add(result)
					//	.add(op.index)
					//	.result;
					result = add_instruction(section, chain.location, spv::OpAccessChain, convert_type(op.to))
						.add(result)
						.add(op.index)
						.result;
					result = add_instruction(section, chain.location, spv::OpLoad, convert_type(op.to))
						.add(result)
						.result;*/
					break;
				}
				else if (op.from.is_vector() && op.to.is_scalar())
				{
					type target_type = op.to;
					target_type.is_ptr = false;

					assert(result != 0);
					result = add_instruction(spv::OpVectorExtractDynamic, convert_type(target_type))
						.add(result) // Vector
						.add(op.index) // Index
						.result; // Result ID
					break;
				}
				assert(false);
				break;
			case expression::operation::op_swizzle:
				if (op.to.is_vector())
				{
					if (op.from.is_matrix())
					{
						spv::Id components[4];

						for (unsigned int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
						{
							const unsigned int row = op.swizzle[i] / 4;
							const unsigned int column = op.swizzle[i] - row * 4;

							type scalar_type = op.to;
							scalar_type.rows = 1;
							scalar_type.cols = 1;

							assert(result != 0);
							spirv_instruction &node = add_instruction(spv::OpCompositeExtract, convert_type(scalar_type))
								.add(result);

							if (op.from.rows > 1) // Matrix types with a single row are actually vectors, so they don't need the extra index
								node.add(row);

							node.add(column);

							components[i] = node.result;
						}

						spirv_instruction &node = add_instruction(spv::OpCompositeConstruct, convert_type(op.to));

						for (unsigned int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
						{
							node.add(components[i]);
						}

						result = node.result;
						break;
					}
					else
					{
						assert(op.from.is_vector());

						spirv_instruction &node = add_instruction(spv::OpVectorShuffle, convert_type(op.to))
							.add(result) // Vector 1
							.add(result); // Vector 2

						for (unsigned int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
							node.add(op.swizzle[i]);

						result = node.result;
						break;
					}
				}
				else if (op.to.is_scalar())
				{
					assert(op.swizzle[1] < 0);

					assert(result != 0);
					spirv_instruction &node = add_instruction(spv::OpCompositeExtract, convert_type(op.to))
						.add(result); // Composite

					if (op.from.is_matrix() && op.from.rows > 1)
					{
						const unsigned int row = op.swizzle[0] / 4;
						const unsigned int column = op.swizzle[0] - row * 4;
						node.add(row);
						node.add(column);
					}
					else
					{
						node.add(op.swizzle[0]);
					}

					result = node.result; // Result ID
					break;
				}
				assert(false);
				break;
			}
		}

		return result;
	}
	void emit_store(const expression &chain, spv::Id value, const type &value_type) override
	{
		assert(value != 0);
		assert(chain.is_lvalue && !chain.is_constant);
		assert(!value_type.is_ptr);

		add_location(chain.location, *_current_block_data);

		spv::Id target = chain.base;

		size_t op_index2 = 0;

		auto base_type = chain.type;
		if (!chain.ops.empty())
			base_type = chain.ops[0].from;

		// Any indexing expressions can be resolved with an 'OpAccessChain' already
		if (!chain.ops.empty() && chain.ops[0].type == expression::operation::op_index)
		{
			assert(chain.ops[0].to.is_ptr);
			spirv_instruction &node = add_instruction(spv::OpAccessChain)
				.add(target); // Base

			// Ignore first index into 1xN matrices, since they were translated to a vector type in SPIR-V
			if (chain.ops[0].from.rows == 1 && chain.ops[0].from.cols > 1)
				op_index2 = 1;

			do {
				assert(chain.ops[op_index2].to.is_ptr);
				base_type = chain.ops[op_index2].to;
				node.add(chain.ops[op_index2++].index); // Indexes
			} while (op_index2 < chain.ops.size() && chain.ops[op_index2].type == expression::operation::op_index);
			node.type = convert_type(chain.ops[op_index2 - 1].to); // Last type is the result
			target = node.result; // Result ID
		}

		// TODO: Complex access chains like float4x4[0].m00m10[0] = 0;
		// Work through all remaining operations in the access chain and apply them to the value
		for (; op_index2 < chain.ops.size(); ++op_index2)
		{
			const auto &op = chain.ops[op_index2];

			switch (op.type)
			{
			case expression::operation::op_cast:
				assert(false); // This cannot happen
				break;
			case expression::operation::op_index:
				assert(false);
				break;
			case expression::operation::op_swizzle:
			{
				base_type.is_ptr = false;

				spv::Id result = add_instruction(spv::OpLoad, convert_type(base_type))
					.add(target) // Pointer
					.result; // Result ID

				if (base_type.is_vector() && value_type.is_vector())
				{
					spirv_instruction &node = add_instruction(spv::OpVectorShuffle, convert_type(base_type))
						.add(result) // Vector 1
						.add(value); // Vector 2

					unsigned int shuffle[4] = { 0, 1, 2, 3 };
					for (unsigned int i = 0; i < base_type.rows; ++i)
						if (op.swizzle[i] >= 0)
							shuffle[op.swizzle[i]] = base_type.rows + i;
					for (unsigned int i = 0; i < base_type.rows; ++i)
						node.add(shuffle[i]);

					value = node.result;
				}
				else if (op.to.is_scalar())
				{
					assert(op.swizzle[1] < 0); // TODO

					spv::Id result2 = add_instruction(spv::OpCompositeInsert, convert_type(base_type))
						.add(value) // Object
						.add(result) // Composite
						.add(op.swizzle[0]) // Index
						.result; // Result ID

					value = result2;
				}
				else
				{
					assert(false);
				}
				break;
			}
			}
		}

		add_instruction_without_result(spv::OpStore)
			.add(target)
			.add(value);
	}

	void set_block(id id) override
	{
		_current_block = id;
		_current_block_data = &_block_data[id];
	}
	void enter_block(id id) override
	{
		// Can only use labels inside functions and should never be in another basic block if creating a new one
		assert(is_in_function() && !is_in_block());

		_current_block = id; // All instructions following a label are inside that basic block
		_current_block_data = &_functions2[_current_function].definition; // &_block_data[id];

		add_instruction_without_result(spv::OpLabel)
			.result = id;
	}
	void leave_block_and_kill() override
	{
		assert(is_in_function()); // Can only discard inside functions

		if (!is_in_block())
			return;

		add_instruction_without_result(spv::OpKill);

		_last_block = _current_block;
		_current_block = 0; // A discard leaves the current basic block
	}
	void leave_block_and_return(id value) override
	{
		assert(is_in_function()); // Can only return from inside functions

		if (!is_in_block()) // Might already have left the last block in which case this has to be ignored
			return;

		if (_functions2[_current_function].return_type.is_void())
		{
			add_instruction_without_result(spv::OpReturn);
		}
		else
		{
			if (value == 0)
				value = add_instruction(spv::OpUndef, convert_type(_functions2[_current_function].return_type), _types_and_constants).result;

			add_instruction_without_result(spv::OpReturnValue)
				.add(value);
		}

		_last_block = _current_block;
		_current_block = 0; // A return leaves the current basic block
	}
	void leave_block_and_switch(id value) override
	{
		assert(is_in_function()); // Can only switch inside functions

		if (!is_in_block())
			return;

		// Default and case labels are added later in 'set_switch_case_labels()'
		add_instruction_without_result(spv::OpSwitch)
			.add(value);

		_last_block = _current_block;
		_current_block = 0; // A switch leaves the current basic block
	}
	void leave_block_and_branch(id target) override
	{
		assert(is_in_function()); // Can only branch inside functions

		if (!is_in_block())
			return;

		add_instruction_without_result(spv::OpBranch)
			.add(target);

		_last_block = _current_block;
		_current_block = 0; // A branch leaves the current basic block
	}
	void leave_block_and_branch_conditional(id condition, id true_target, id false_target) override
	{
		assert(is_in_function()); // Can only branch inside functions

		if (!is_in_block())
			return;

		add_instruction_without_result(spv::OpBranchConditional)
			.add(condition)
			.add(true_target)
			.add(false_target);

		_last_block = _current_block;
		_current_block = 0; // A branch leaves the current basic block
	}

	void enter_function(id id, const type &ret_type) override
	{
		auto &function = _functions2.emplace_back();
		function.return_type = ret_type;

		_current_function = _functions2.size() - 1;

		spirv_instruction &instruction = add_instruction_without_result(spv::OpFunction, function.declaration);
		instruction.op = spv::OpFunction;
		instruction.type = convert_type(ret_type);
		instruction.result = id;
		instruction.add(spv::FunctionControlMaskNone);
	}
	void leave_function() override
	{
		assert(is_in_function()); // Can only leave if there was a function to begin with

		auto &function = _functions2[_current_function];
		//function.definition = _block_data[_last_block];

		// Append function end instruction
		add_instruction_without_result(spv::OpFunctionEnd, function.definition);

		// Now that all parameters are known, the full function type can be added to the function
		function.declaration.instructions[0].add(convert_type(function)); // Function Type

		_current_function = 0xFFFFFFFF;
	}

public:
	codegen_spirv()
	{
		glsl_ext = make_id();
	}
};

codegen *create_codegen_spirv()
{
	return new codegen_spirv();
}
