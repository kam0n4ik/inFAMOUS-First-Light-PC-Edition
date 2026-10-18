// SecondSonPC — the SPIR-V module builder (spirv_builder.h).
#include "runtime/gpu/gcn/spirv_builder.h"

#include <bit>
#include <cstring>

namespace SecondSon::Gpu::Gcn {

SpirvBuilder::SpirvBuilder() = default;

void SpirvBuilder::Append(std::vector<u32>& s, spv::Op op, std::span<const u32> words) {
    s.push_back((static_cast<u32>(words.size() + 1) << 16) | static_cast<u32>(op));
    s.insert(s.end(), words.begin(), words.end());
}

void SpirvBuilder::AppendString(std::vector<u32>& words, std::string_view s) {
    const size_t n = s.size() / 4 + 1; // (always a terminating zero byte)
    const size_t at = words.size();
    words.resize(at + n, 0);
    std::memcpy(words.data() + at, s.data(), s.size());
}

void SpirvBuilder::Capability(spv::Capability c) {
    const u32 w = static_cast<u32>(c);
    for (size_t i = 0; i < capabilities_.size(); i += 2) {
        if (capabilities_[i + 1] == w) {
            return;
        }
    }
    Append(capabilities_, spv::Op::OpCapability, std::span<const u32>(&w, 1));
}

void SpirvBuilder::Extension(std::string_view name) {
    std::vector<u32> w;
    AppendString(w, name);
    Append(extensions_, spv::Op::OpExtension, w);
}

Id SpirvBuilder::ExtInstImport(std::string_view name) {
    const Id id = NewId();
    std::vector<u32> w{id};
    AppendString(w, name);
    Append(imports_, spv::Op::OpExtInstImport, w);
    return id;
}

void SpirvBuilder::MemoryModel(spv::AddressingModel a, spv::MemoryModel m) {
    memory_model_.clear();
    const u32 w[2] = {static_cast<u32>(a), static_cast<u32>(m)};
    Append(memory_model_, spv::Op::OpMemoryModel, w);
}

void SpirvBuilder::EntryPoint(spv::ExecutionModel model, Id function, std::string_view name,
                              std::span<const Id> interface) {
    std::vector<u32> w{static_cast<u32>(model), function};
    AppendString(w, name);
    w.insert(w.end(), interface.begin(), interface.end());
    Append(entry_points_, spv::Op::OpEntryPoint, w);
}

void SpirvBuilder::ExecutionMode(Id function, spv::ExecutionMode mode, std::initializer_list<u32> args) {
    std::vector<u32> w{function, static_cast<u32>(mode)};
    w.insert(w.end(), args.begin(), args.end());
    Append(execution_modes_, spv::Op::OpExecutionMode, w);
}

void SpirvBuilder::Name(Id id, std::string_view name) {
    std::vector<u32> w{id};
    AppendString(w, name);
    Append(names_, spv::Op::OpName, w);
}

void SpirvBuilder::Decorate(Id id, spv::Decoration d, std::initializer_list<u32> args) {
    std::vector<u32> w{id, static_cast<u32>(d)};
    w.insert(w.end(), args.begin(), args.end());
    Append(decorations_, spv::Op::OpDecorate, w);
}

void SpirvBuilder::MemberDecorate(Id type, u32 member, spv::Decoration d, std::initializer_list<u32> args) {
    std::vector<u32> w{type, member, static_cast<u32>(d)};
    w.insert(w.end(), args.begin(), args.end());
    Append(decorations_, spv::Op::OpMemberDecorate, w);
}

Id SpirvBuilder::Dedup(spv::Op op, std::vector<u32> operands) {
    std::vector<u32> key{static_cast<u32>(op)};
    key.insert(key.end(), operands.begin(), operands.end());
    if (const auto it = dedup_.find(key); it != dedup_.end()) {
        return it->second;
    }
    const Id id = NewId();
    // a constant's result type comes before its result id; a type has only the result id
    const bool constant = op == spv::Op::OpConstant || op == spv::Op::OpConstantComposite ||
                          op == spv::Op::OpConstantTrue || op == spv::Op::OpConstantFalse;
    std::vector<u32> w;
    if (constant) {
        w.push_back(operands[0]);
        w.push_back(id);
        w.insert(w.end(), operands.begin() + 1, operands.end());
    } else {
        w.push_back(id);
        w.insert(w.end(), operands.begin(), operands.end());
    }
    Append(globals_, op, w);
    dedup_.emplace(std::move(key), id);
    return id;
}

Id SpirvBuilder::TypeVoid() {
    return Dedup(spv::Op::OpTypeVoid, {});
}
Id SpirvBuilder::TypeBool() {
    return Dedup(spv::Op::OpTypeBool, {});
}
Id SpirvBuilder::TypeInt(u32 width, bool is_signed) {
    return Dedup(spv::Op::OpTypeInt, {width, is_signed ? 1u : 0u});
}
Id SpirvBuilder::TypeFloat(u32 width) {
    return Dedup(spv::Op::OpTypeFloat, {width});
}
Id SpirvBuilder::TypeVector(Id component, u32 count) {
    return Dedup(spv::Op::OpTypeVector, {component, count});
}
Id SpirvBuilder::TypeArray(Id element, Id length_constant) {
    return Dedup(spv::Op::OpTypeArray, {element, length_constant});
}
Id SpirvBuilder::TypeRuntimeArray(Id element) {
    const Id id = NewId();
    const u32 w[2] = {id, element};
    Append(globals_, spv::Op::OpTypeRuntimeArray, w);
    return id;
}
Id SpirvBuilder::TypeStruct(std::span<const Id> members) {
    const Id id = NewId();
    std::vector<u32> w{id};
    w.insert(w.end(), members.begin(), members.end());
    Append(globals_, spv::Op::OpTypeStruct, w);
    return id;
}
Id SpirvBuilder::TypePointer(spv::StorageClass storage, Id type) {
    return Dedup(spv::Op::OpTypePointer, {static_cast<u32>(storage), type});
}
Id SpirvBuilder::TypeFunction(Id result, std::span<const Id> params) {
    std::vector<u32> w{result};
    w.insert(w.end(), params.begin(), params.end());
    return Dedup(spv::Op::OpTypeFunction, std::move(w));
}

Id SpirvBuilder::Constant(Id type, u32 value) {
    return Dedup(spv::Op::OpConstant, {type, value});
}
Id SpirvBuilder::ConstantBool(bool value) {
    return Dedup(value ? spv::Op::OpConstantTrue : spv::Op::OpConstantFalse, {TypeBool()});
}
Id SpirvBuilder::ConstantComposite(Id type, std::span<const Id> parts) {
    std::vector<u32> w{type};
    w.insert(w.end(), parts.begin(), parts.end());
    return Dedup(spv::Op::OpConstantComposite, std::move(w));
}
Id SpirvBuilder::F32(float v) {
    return Constant(TypeFloat(32), std::bit_cast<u32>(v));
}

Id SpirvBuilder::Variable(Id pointer_type, spv::StorageClass storage, Id initializer) {
    const Id id = NewId();
    std::vector<u32> w{pointer_type, id, static_cast<u32>(storage)};
    if (initializer != 0) {
        w.push_back(initializer);
    }
    Append(globals_, spv::Op::OpVariable, w);
    return id;
}

Id SpirvBuilder::BeginFunction(Id result_type, Id function_type, spv::FunctionControlMask control) {
    const Id id = NewId();
    function_head_.clear();
    function_vars_.clear();
    function_body_.clear();
    const u32 w[4] = {result_type, id, static_cast<u32>(control), function_type};
    Append(function_head_, spv::Op::OpFunction, w);
    first_block_open_ = false;
    return id;
}

Id SpirvBuilder::LocalVariable(Id pointer_type, Id initializer) {
    const Id id = NewId();
    std::vector<u32> w{pointer_type, id, static_cast<u32>(spv::StorageClass::Function)};
    if (initializer != 0) {
        w.push_back(initializer);
    }
    Append(function_vars_, spv::Op::OpVariable, w);
    return id;
}

void SpirvBuilder::Label(Id label) {
    const u32 w = label;
    if (!first_block_open_) {
        // the first block: its label, then the function's variables, then its instructions
        Append(function_head_, spv::Op::OpLabel, std::span<const u32>(&w, 1));
        first_block_open_ = true;
        return;
    }
    Append(function_body_, spv::Op::OpLabel, std::span<const u32>(&w, 1));
}

Id SpirvBuilder::Op(spv::Op op, Id result_type, std::span<const u32> operands, bool has_result) {
    std::vector<u32> w;
    Id id = 0;
    if (result_type != 0) {
        w.push_back(result_type);
    }
    if (has_result) {
        id = NewId();
        w.push_back(id);
    }
    w.insert(w.end(), operands.begin(), operands.end());
    Append(function_body_, op, w);
    return id;
}

Id SpirvBuilder::Op(spv::Op op, Id result_type, std::initializer_list<u32> operands, bool has_result) {
    return Op(op, result_type, std::span<const u32>(operands.begin(), operands.size()), has_result);
}

void SpirvBuilder::OpNoResult(spv::Op op, std::initializer_list<u32> operands) {
    Op(op, 0, operands, false);
}

void SpirvBuilder::EndFunction() {
    functions_.insert(functions_.end(), function_head_.begin(), function_head_.end());
    functions_.insert(functions_.end(), function_vars_.begin(), function_vars_.end());
    functions_.insert(functions_.end(), function_body_.begin(), function_body_.end());
    Append(functions_, spv::Op::OpFunctionEnd, {});
    function_head_.clear();
    function_vars_.clear();
    function_body_.clear();
}

std::vector<u32> SpirvBuilder::Assemble() const {
    std::vector<u32> out{spv::MagicNumber, 0x00010600, 0, next_id_, 0};
    for (const auto* s : {&capabilities_, &extensions_, &imports_, &memory_model_, &entry_points_, &execution_modes_,
                          &names_, &decorations_, &globals_, &functions_}) {
        out.insert(out.end(), s->begin(), s->end());
    }
    return out;
}

} // namespace SecondSon::Gpu::Gcn
