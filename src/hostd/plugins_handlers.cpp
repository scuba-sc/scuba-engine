#include "hostd/modules.h"
#include "sco/runtime.h"
#include "sco/caps.h"

#include <array>
#include <cmath>

namespace scuba::hostd {
namespace {
struct Reply {
    bool called = false;
    std::string text;
};

void OnReply(sco::Result, const char* text, void* context) {
    auto& reply = *static_cast<Reply*>(context);
    reply.called = true;
    reply.text = text ? text : "";
}
}  // namespace

void RegisterPluginHandlers(Registry& registry, HostContext&) {
    registry.register_handler("plugins.commands", [](const Json&, const CallContext&) {
        std::array<const sco::Command*, sco::kMaxCommands> commands{};
        const size_t count = sco::ListCommands(commands.data(), commands.size());
        Json out = Json::MakeArray();
        for (size_t i = 0; i < count; ++i) {
            const auto& command = *commands[i];
            Json item = Json::MakeObject();
            item.set("name", command.name);
            item.set("title", command.title);
            item.set("help", command.help ? command.help : "");
            item.set("capability", command.capability ? command.capability : "");
            item.set("ready", !command.capability || sco::caps::Has(command.capability));
            Json args = Json::MakeArray();
            for (uint32_t j = 0; j < command.nargs; ++j) {
                const auto& arg = command.args[j];
                Json definition = Json::MakeObject();
                definition.set("name", arg.name);
                definition.set("help", arg.help ? arg.help : "");
                switch (arg.type) {
                    case sco::ArgType::Int: definition.set("type", "integer"); break;
                    case sco::ArgType::Float: definition.set("type", "number"); break;
                    case sco::ArgType::String: definition.set("type", "string"); break;
                    case sco::ArgType::Bool: definition.set("type", "boolean"); break;
                }
                args.push_back(std::move(definition));
            }
            item.set("args", std::move(args));
            out.push_back(std::move(item));
        }
        return out;
    });
    registry.register_handler("plugins.invoke", [](const Json& params, const CallContext&) {
        const Json* name = params.find("name");
        const Json* values = params.find("args");
        if (!name || !name->is_string() || (values && !values->is_array()))
            throw HandlerError(rpc_error::kInvalidParams, "expected name and optional args array");
        std::array<const sco::Command*, sco::kMaxCommands> commands{};
        const size_t count = sco::ListCommands(commands.data(), commands.size());
        const sco::Command* command = nullptr;
        for (size_t i = 0; i < count; ++i)
            if (name->as_string() == commands[i]->name) command = commands[i];
        if (!command) throw HandlerError(rpc_error::kMethodNotFound, "no such plugin command");
        if ((values ? values->size() : 0) != command->nargs)
            throw HandlerError(rpc_error::kInvalidParams, "wrong command argument count");
        std::array<sco::Arg, sco::kMaxCommandArgs> args{};
        for (uint32_t i = 0; i < command->nargs; ++i) {
            const Json& value = values->at(i);
            auto& arg = args[i];
            arg.type = command->args[i].type;
            bool valid = false;
            switch (arg.type) {
                case sco::ArgType::String:
                    valid = value.is_string() && value.as_string().find('\0') == std::string::npos;
                    if (valid) arg.v.s = value.as_string().c_str();
                    break;
                case sco::ArgType::Bool:
                    valid = value.is_bool();
                    if (valid) arg.v.i = value.as_bool() ? 1 : 0;
                    break;
                case sco::ArgType::Int: {
                    const double number = value.as_number();
                    valid = value.is_number() && std::isfinite(number) && std::floor(number) == number &&
                            std::abs(number) <= 9007199254740991.0;
                    if (valid) arg.v.i = static_cast<int64_t>(number);
                    break;
                }
                case sco::ArgType::Float:
                    valid = value.is_number() && std::isfinite(value.as_number());
                    if (valid) arg.v.f = value.as_number();
                    break;
            }
            if (!valid) throw HandlerError(rpc_error::kInvalidParams, "wrong command argument type");
        }
        Reply reply;
        const sco::Result result = sco::Invoke(command->name, args.data(), command->nargs, OnReply, &reply);
        if (!reply.called) throw HandlerError(rpc_error::kHandlerFailed, "command did not reply");
        Json out = Json::MakeObject();
        out.set("result", sco::ResultName(result));
        out.set("reply", reply.text);
        return out;
    });
}
}  // namespace scuba::hostd
