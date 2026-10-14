#include "assets/datacore.h"
#include <algorithm>

namespace scuba::assets {
namespace dc = sco::datacore;
using hostd::Json;
namespace {
dc::RecordRef RecordRef(std::string_view name) {
    dc::RecordRef result;
    dc::Guid guid;
    const auto value = name.starts_with("guid:") ? name.substr(5) : name;
    if (dc::ParseGuid(value, guid)) result.guid = guid;
    else result.name = name;
    return result;
}
Json RecordJson(const dc::Schema& schema, const dc::Record& record) {
    return Json::MakeObject().set("name", schema.Name(record.name))
        .set("guid", dc::FormatGuid(record.id)).set("struct", schema.StructName(record.structIndex))
        .set("file", schema.ValueString(record.fileName));
}
dc::Pack Parse(std::string_view text) {
    dc::Pack pack;
    pack.plugin = "asset-lab";
    pack.name = "datacore/overrides.toml";
    std::string error;
    if (!dc::ParsePack(text, pack, error)) throw Error("pack refused: " + error);
    return pack;
}
}
Json DataCore::Open(const std::filesystem::path& path) {
    auto bytes = ReadFile(path, path.extension() == ".toml" ? dc::kMaxPackBytes : size_t{2} * 1024 * 1024 * 1024);
    if (path.extension() == ".toml") {
        auto pack = Parse(Text(bytes));
        pack_ = std::move(pack);
        bytes_.clear();
        schema_ = dc::Schema{};
        loaded_ = false;
    } else if (path.extension() == ".dcb") {
        dc::Schema schema;
        if (!schema.Parse(bytes)) throw Error("DataCore layout refused: " + schema.error);
        // Schema contains views into bytes: moving the owning vector retains their addresses.
        bytes_ = std::move(bytes);
        schema_ = std::move(schema);
        loaded_ = true;
        pack_ = Parse("format = 1\natomic = true\n");
    } else throw Error("DataCore input must be .dcb or .toml");
    return Json::MakeObject().set("kind", loaded_ ? "dcb" : "toml")
        .set("records", loaded_ ? schema_.records.size() : pack_.ops.size())
        .set("structs", loaded_ ? schema_.structs.size() : 0)
        .set("pack", Export());
}
Json DataCore::Records(size_t offset, size_t limit, std::string_view filter) const {
    Json items = Json::MakeArray();
    size_t matched = 0;
    if (loaded_) {
        for (const auto& record : schema_.records) {
            const auto name = schema_.Name(record.name);
            if (name.find(filter) == std::string_view::npos && schema_.StructName(record.structIndex).find(filter) == std::string_view::npos &&
                dc::FormatGuid(record.id).find(filter) == std::string::npos) continue;
            if (matched++ >= offset && items.size() < limit) items.push_back(RecordJson(schema_, record));
        }
    } else {
        for (const auto& op : pack_.ops) {
            const auto name = op.record ? op.record->name : op.id;
            if (name.find(filter) == std::string::npos) continue;
            if (matched++ >= offset && items.size() < limit)
                items.push_back(Json::MakeObject().set("name", name)
                    .set("guid", op.record && op.record->guid ? dc::FormatGuid(*op.record->guid) : "")
                    .set("struct", op.type).set("field", op.field).set("line", op.line));
        }
    }
    return Json::MakeObject().set("items", std::move(items)).set("total", matched).set("offset", offset);
}
Json DataCore::Structs() const {
    Json items = Json::MakeArray();
    if (!loaded_) {
        for (const auto& op : pack_.ops)
            if (!op.type.empty()) items.push_back(Json::MakeObject().set("name", op.type).set("fromPack", true));
        return items;
    }
    for (size_t index = 0; index < schema_.structs.size(); ++index) {
        const auto& definition = schema_.structs[index];
        Json properties = Json::MakeArray();
        for (const auto propertyIndex : schema_.Properties(static_cast<uint32_t>(index))) {
            const auto& property = schema_.properties[propertyIndex];
            properties.push_back(Json::MakeObject().set("name", schema_.Name(property.name))
                .set("type", dc::TypeName(property.dataType)).set("conversion", property.conversion)
                .set("typeIndex", property.typeIndex));
        }
        items.push_back(Json::MakeObject().set("name", schema_.StructName(static_cast<uint32_t>(index)))
            .set("parent", definition.parent).set("opaque", schema_.structInfo[index].opaque)
            .set("properties", std::move(properties)));
    }
    return items;
}
Json DataCore::Fields(std::string_view recordName) const {
    if (!loaded_) throw Error("field values require a .dcb; TOML lists semantic operations only");
    dc::Patch patch(schema_);
    const auto reference = RecordRef(recordName);
    std::vector<dc::FieldView> fields;
    if (const auto status = patch.ReadFields(reference, "", fields, 8); !status) throw Error(status.message);
    Json items = Json::MakeArray();
    for (const auto& field : fields) {
        Json item = Json::MakeObject().set("path", field.path).set("type", dc::TypeName(field.dataType))
            .set("value", field.text).set("array", field.array).set("depth", field.depth);
        // Reference values use the kit's pack form { ref = "guid:..." }; resolve by GUID, never table position.
        const auto marker = field.text.find("guid:");
        if (marker != std::string::npos && field.text.size() - marker >= 41) {
            dc::Guid guid;
            const auto text = field.text.substr(marker + 5, 36);
            if (dc::ParseGuid(text, guid))
                if (const auto* target = schema_.FindRecord(guid)) item.set("reference", RecordJson(schema_, *target));
        }
        items.push_back(std::move(item));
    }
    return items;
}
void DataCore::SetPack(std::string_view text) {
    auto pack = Parse(text);
    if (loaded_) {
        const auto result = dc::ApplyPacks(schema_, std::span<const dc::Pack>(&pack, 1));
        if (!result.status) throw Error(result.status.message);
        for (const auto& report : result.packs)
            if (report.state != dc::PackState::Applied) throw Error(report.reason);
    }
    pack_ = std::move(pack);
}
std::string DataCore::Export() const { return dc::WritePack(pack_); }
} // namespace scuba::assets
