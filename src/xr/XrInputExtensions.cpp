#include "xr/XrInputExtensions.hpp"
#include "xr/XrMath.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace saida::xr {
namespace {
void require(XrResult result, const char* operation) {
    if (XR_FAILED(result))
        throw std::runtime_error(std::string("OpenXR input extension: ") + operation + " failed (" +
                                 std::to_string(result) + ")");
}
XrPath path(XrInstance instance, const std::string& name) {
    XrPath result = XR_NULL_PATH;
    require(xrStringToPath(instance, name.c_str(), &result), "xrStringToPath");
    return result;
}
bool text(const std::string& value, size_t maximum) {
    return !value.empty() && value.size() < maximum && value.find('\0') == std::string::npos;
}
XrActionType actionType(InputType type) {
    switch (type) {
    case InputType::Boolean:
        return XR_ACTION_TYPE_BOOLEAN_INPUT;
    case InputType::Float:
        return XR_ACTION_TYPE_FLOAT_INPUT;
    case InputType::Vector2:
        return XR_ACTION_TYPE_VECTOR2F_INPUT;
    case InputType::Pose:
        return XR_ACTION_TYPE_POSE_INPUT;
    }
    throw std::invalid_argument("OpenXR input extension: invalid action type");
}
} // namespace

void InputExtensions::validate(const InputExtensionDescription& description) {
    if (!text(description.name, 128) || description.actions.size() > kMaximumActionsPerProvider ||
        description.requiredExtensions.size() > 64)
        throw std::invalid_argument("OpenXR input extension: invalid description");
    std::set<std::string> names;
    for (const auto& extension : description.requiredExtensions)
        if (!text(extension, XR_MAX_EXTENSION_NAME_SIZE) || !names.insert(extension).second)
            throw std::invalid_argument("OpenXR input extension: invalid/duplicate extension dependency");
    names.clear();
    size_t bindings = 0;
    for (const auto& action : description.actions) {
        actionType(action.type);
        if (!text(action.name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE) || !names.insert(action.name).second ||
            action.bindings.empty())
            throw std::invalid_argument("OpenXR input extension: invalid/duplicate action");
        std::set<std::pair<std::string, std::string>> sources;
        for (const auto& binding : action.bindings) {
            if (++bindings > kMaximumBindingsPerProvider || !text(binding.profile, XR_MAX_PATH_LENGTH) ||
                !text(binding.userPath, XR_MAX_PATH_LENGTH) ||
                !text(binding.componentPath, XR_MAX_PATH_LENGTH) ||
                binding.profile.rfind("/interaction_profiles/", 0) != 0 ||
                binding.userPath.rfind("/user/", 0) != 0 ||
                binding.componentPath.rfind(binding.userPath + "/input/", 0) != 0 ||
                !sources.emplace(binding.profile, binding.componentPath).second)
                throw std::invalid_argument("OpenXR input extension: invalid/duplicate binding");
        }
    }
}

InputExtensions::InputExtensions(InputExtensionList providers) {
    if (providers.size() > kMaximumProviders)
        throw std::length_error("OpenXR input extension: provider capacity reached");
    std::set<InputExtension*> identities;
    std::set<std::string> names;
    providers_.reserve(providers.size());
    for (auto& target : providers) {
        if (!target || !identities.insert(target.get()).second)
            throw std::invalid_argument("OpenXR input extension: null/duplicate provider");
        auto description = target->describe();
        validate(description);
        if (!names.insert(description.name).second)
            throw std::invalid_argument("OpenXR input extension: duplicate provider name");
        Provider provider;
        provider.target = std::move(target);
        provider.description = std::move(description);
        providers_.push_back(std::move(provider));
    }
}
InputExtensions::~InputExtensions() {
    releaseSession();
}

void InputExtensions::negotiate(const std::vector<XrExtensionProperties>& available,
                                std::vector<const char*>& enabled) {
    if (sessionInitialized_)
        throw std::logic_error("OpenXR input extension: cannot renegotiate a live session");
    for (auto& provider : providers_) {
        provider.enabled =
            std::all_of(provider.description.requiredExtensions.begin(),
                        provider.description.requiredExtensions.end(), [&](const std::string& name) {
                            return std::any_of(available.begin(), available.end(),
                                               [&](const XrExtensionProperties& extension) {
                                                   return name == extension.extensionName;
                                               });
                        });
        if (!provider.enabled) {
            Log::info("XR input extension ", provider.description.name, ": required extensions unavailable");
            continue;
        }
        for (const auto& name : provider.description.requiredExtensions)
            if (std::none_of(enabled.begin(), enabled.end(),
                             [&](const char* existing) { return name == existing; }))
                enabled.push_back(name.c_str());
    }
}

void InputExtensions::createAction(Provider& provider, size_t providerIndex, size_t actionIndex,
                                   XrInstance instance, XrActionSet set, XrSession session,
                                   const CollectBinding& collect) {
    const auto& description = provider.description.actions[actionIndex];
    std::vector<XrPath> users;
    std::vector<std::string_view> userNames;
    for (const auto& binding : description.bindings) {
        const XrPath user = path(instance, binding.userPath);
        if (std::find(users.begin(), users.end(), user) == users.end()) {
            users.push_back(user);
            userNames.push_back(binding.userPath);
        }
    }
    XrActionCreateInfo create{};
    create.type = XR_TYPE_ACTION_CREATE_INFO;
    std::snprintf(create.actionName, sizeof(create.actionName), "extension_%zu_%zu", providerIndex,
                  actionIndex);
    // Localized names must also be unique within the shared action set. Use
    // the generated identifier to avoid cross-provider clashes or truncating
    // an arbitrary UTF-8 label in the middle of a code point.
    std::strncpy(create.localizedActionName, create.actionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    create.actionType = actionType(description.type);
    create.countSubactionPaths = static_cast<uint32_t>(users.size());
    create.subactionPaths = users.data();
    XrAction action = XR_NULL_HANDLE;
    require(xrCreateAction(set, &create, &action), "xrCreateAction");
    provider.actions.push_back(action);
    for (size_t i = 0; i < users.size(); ++i) {
        provider.channels.push_back(
            {actionIndex, description.type, action, users[i], userNames[i], XR_NULL_HANDLE});
        auto& channel = provider.channels.back();
        if (description.type == InputType::Pose) {
            XrActionSpaceCreateInfo space{};
            space.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
            space.action = action;
            space.subactionPath = users[i];
            space.poseInActionSpace.orientation.w = 1;
            require(xrCreateActionSpace(session, &space, &channel.space), "xrCreateActionSpace");
        }
    }
    for (const auto& binding : description.bindings) {
        const XrPath profile = path(instance, binding.profile);
        if (std::find(provider.profiles.begin(), provider.profiles.end(), profile) == provider.profiles.end())
            provider.profiles.push_back(profile);
        collect(binding.profile, {action, path(instance, binding.componentPath)});
    }
}

void InputExtensions::createActions(XrInstance instance, XrActionSet set, XrSession session,
                                    const CollectBinding& collect) {
    if (sessionInitialized_)
        throw std::logic_error("OpenXR input extension: session already initialized");
    sessionInitialized_ = true;
    try {
        for (size_t i = 0; i < providers_.size(); ++i) {
            auto& provider = providers_[i];
            if (!provider.enabled)
                continue;
            provider.actions.reserve(provider.description.actions.size());
            size_t bindingCount = 0;
            for (const auto& action : provider.description.actions)
                bindingCount += action.bindings.size();
            provider.channels.reserve(bindingCount);
            provider.profiles.reserve(bindingCount);
            provider.acceptedProfiles.reserve(bindingCount);
            for (size_t j = 0; j < provider.description.actions.size(); ++j)
                createAction(provider, i, j, instance, set, session, collect);
            provider.values.resize(provider.channels.size());
        }
    } catch (...) {
        releaseSession();
        throw;
    }
}

void InputExtensions::profileResult(XrPath profile, bool accepted) {
    for (auto& provider : providers_)
        if (std::find(provider.profiles.begin(), provider.profiles.end(), profile) !=
            provider.profiles.end()) {
            auto found =
                std::find(provider.acceptedProfiles.begin(), provider.acceptedProfiles.end(), profile);
            if (accepted && found == provider.acceptedProfiles.end())
                provider.acceptedProfiles.push_back(profile);
            if (!accepted && found != provider.acceptedProfiles.end())
                provider.acceptedProfiles.erase(found);
            provider.bound = !provider.acceptedProfiles.empty();
            if (!accepted)
                Log::warn("XR input extension ", provider.description.name, ": profile bindings rejected");
        }
}

InputValue InputExtensions::read(const Channel& channel, XrSession session, XrSpace space,
                                 XrTime time) noexcept {
    InputValue value;
    value.action = channel.actionIndex;
    value.userPath = channel.userPath;
    XrActionStateGetInfo get{};
    get.type = XR_TYPE_ACTION_STATE_GET_INFO;
    get.action = channel.action;
    get.subactionPath = channel.user;
    if (channel.type == InputType::Boolean) {
        XrActionStateBoolean state{};
        state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &state)) && state.isActive) {
            value.active = true;
            value.boolean = state.currentState == XR_TRUE;
            value.changed = state.changedSinceLastSync == XR_TRUE;
        }
    } else if (channel.type == InputType::Float) {
        XrActionStateFloat state{};
        state.type = XR_TYPE_ACTION_STATE_FLOAT;
        if (XR_SUCCEEDED(xrGetActionStateFloat(session, &get, &state)) && state.isActive &&
            std::isfinite(state.currentState)) {
            value.active = true;
            value.scalar = state.currentState;
            value.changed = state.changedSinceLastSync;
        }
    } else if (channel.type == InputType::Vector2) {
        XrActionStateVector2f state{};
        state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &get, &state)) && state.isActive &&
            std::isfinite(state.currentState.x) && std::isfinite(state.currentState.y)) {
            value.active = true;
            value.vector = {state.currentState.x, state.currentState.y};
            value.changed = state.changedSinceLastSync;
        }
    } else {
        XrActionStatePose state{};
        state.type = XR_TYPE_ACTION_STATE_POSE;
        if (XR_SUCCEEDED(xrGetActionStatePose(session, &get, &state)) && state.isActive) {
            XrSpaceLocation location{};
            location.type = XR_TYPE_SPACE_LOCATION;
            if (XR_SUCCEEDED(xrLocateSpace(channel.space, space, time, &location))) {
                value.active = true;
                value.pose.tracked = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                                     (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
                if (value.pose.tracked) {
                    const auto position = toGlm(location.pose.position);
                    const auto orientation = toGlm(location.pose.orientation);
                    const float norm = glm::dot(orientation, orientation);
                    value.pose.tracked = std::isfinite(position.x) && std::isfinite(position.y) &&
                                         std::isfinite(position.z) && std::isfinite(orientation.x) &&
                                         std::isfinite(orientation.y) && std::isfinite(orientation.z) &&
                                         std::isfinite(orientation.w) && std::isfinite(norm) && norm > 1e-8f;
                    if (value.pose.tracked) {
                        value.pose.position = position;
                        value.pose.orientation = glm::normalize(orientation);
                    }
                }
            }
        }
    }
    return value;
}

void InputExtensions::sync(XrSession session, XrSpace space, XrTime time, bool focused) noexcept {
    for (auto& provider : providers_) {
        const bool available = provider.enabled && provider.bound;
        for (size_t i = 0; i < provider.channels.size(); ++i) {
            if (focused && available)
                provider.values[i] = read(provider.channels[i], session, space, time);
            else {
                provider.values[i] = {};
                provider.values[i].action = provider.channels[i].actionIndex;
                provider.values[i].userPath = provider.channels[i].userPath;
            }
        }
        provider.target->submit(
            {available, focused, double(time) * 1e-9, provider.values.data(), provider.values.size()});
    }
}

void InputExtensions::releaseSession() noexcept {
    if (!sessionInitialized_)
        return;
    sessionInitialized_ = false;
    for (auto& provider : providers_) {
        provider.target->submit({});
        for (auto& channel : provider.channels)
            if (channel.space != XR_NULL_HANDLE)
                xrDestroySpace(channel.space);
        for (XrAction action : provider.actions)
            xrDestroyAction(action);
        provider.channels.clear();
        provider.values.clear();
        provider.actions.clear();
        provider.profiles.clear();
        provider.acceptedProfiles.clear();
        provider.bound = false;
    }
}

} // namespace saida::xr
