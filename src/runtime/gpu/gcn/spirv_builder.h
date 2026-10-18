// SecondSonPC — the SPIR-V module builder of SecondSonPC's own shader translator (docs/OWN_SHADER_TRANSLATOR.md, step 1).
// It keeps the logical layout the specification orders (capabilities, extensions, imports, memory model, entry points,
// execution modes, names, decorations, types / constants / globals, functions), gives each type and constant one id
// (structurally equal ones are the same id; structs and runtime arrays are always new), and writes the words.
// Opcodes and enums are Khronos' (third_party/spirv_headers, spirv.hpp11).
#pragma once

#include <initializer_list>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include <spirv/unified1/spirv.hpp11>

#include "base/abi.h"

namespace SecondSon::Gpu::Gcn {

using Id = u32;

class SpirvBuilder {
public:
    SpirvBuilder();

    Id NewId() {
        return next_id_++;
    }

    // ---- module level
    void Capability(spv::Capability c);
    void Extension(std::string_view name);
    Id ExtInstImport(std::string_view name);
    void MemoryModel(spv::AddressingModel a, spv::MemoryModel m);
    void EntryPoint(spv::ExecutionModel model, Id function, std::string_view name, std::span<const Id> interface);
    void ExecutionMode(Id function, spv::ExecutionMode mode, std::initializer_list<u32> args = {});
    void Name(Id id, std::string_view name);
    void Decorate(Id id, spv::Decoration d, std::initializer_list<u32> args = {});
    void MemberDecorate(Id type, u32 member, spv::Decoration d, std::initializer_list<u32> args = {});

    // ---- types (deduplicated, except structs and runtime arrays)
    Id TypeVoid();
    Id TypeBool();
    Id TypeInt(u32 width, bool is_signed);
    Id TypeFloat(u32 width);
    Id TypeVector(Id component, u32 count);
    Id TypeArray(Id element, Id length_constant);
    Id TypeRuntimeArray(Id element);
    Id TypeStruct(std::span<const Id> members);
    Id TypePointer(spv::StorageClass storage, Id type);
    Id TypeFunction(Id result, std::span<const Id> params = {});

    // ---- constants (deduplicated)
    Id Constant(Id type, u32 value);
    Id ConstantBool(bool value);
    Id ConstantComposite(Id type, std::span<const Id> parts);
    Id U32(u32 v) {
        return Constant(TypeInt(32, false), v);
    }
    Id F32(float v);

    /// A module-scope variable (Private, Input, Output, StorageBuffer, PushConstant, Workgroup, ...).
    Id Variable(Id pointer_type, spv::StorageClass storage, Id initializer = 0);

    // ---- functions
    Id BeginFunction(Id result_type, Id function_type,
                     spv::FunctionControlMask control = spv::FunctionControlMask::MaskNone);
    void EndFunction();
    /// A function-scope variable (placed at the start of the function's first block, as required).
    Id LocalVariable(Id pointer_type, Id initializer = 0);
    /// Starts block `label` (an id from NewId: labels are usually referred to before their block exists).
    void Label(Id label);
    /// An instruction in the current block: result type (0: none), a new result id when `has_result`, operands.
    Id Op(spv::Op op, Id result_type, std::initializer_list<u32> operands, bool has_result = true);
    Id Op(spv::Op op, Id result_type, std::span<const u32> operands, bool has_result = true);
    void OpNoResult(spv::Op op, std::initializer_list<u32> operands);

    /// The module's words.
    std::vector<u32> Assemble() const;

private:
    static void Append(std::vector<u32>& s, spv::Op op, std::span<const u32> words);
    static void AppendString(std::vector<u32>& words, std::string_view s);
    Id Dedup(spv::Op op, std::vector<u32> operands);

    Id next_id_ = 1;
    std::vector<u32> capabilities_, extensions_, imports_, memory_model_, entry_points_, execution_modes_, names_,
        decorations_, globals_, functions_;
    std::map<std::vector<u32>, Id> dedup_; ///< (opcode, operands) of a type/constant -> its id
    // the current function: its first block's variables go right after that block's label
    std::vector<u32> function_head_, function_vars_, function_body_;
    bool first_block_open_ = false;
};

} // namespace SecondSon::Gpu::Gcn
