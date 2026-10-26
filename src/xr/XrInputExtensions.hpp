#pragma once

#include "xr/XrInputExtension.hpp"
#include <openxr/openxr.h>
#include <functional>

namespace saida::xr {

// Instance-owned descriptions, session-owned action handles. Session actions
// explicitly release native handles before their parent action set disappears.
class InputExtensions {
public:
    static constexpr size_t kMaximumProviders = 16;
    static constexpr size_t kMaximumActionsPerProvider = 64;
    static constexpr size_t kMaximumBindingsPerProvider = 256;
    explicit InputExtensions(InputExtensionList providers = {});
    ~InputExtensions();
    InputExtensions(const InputExtensions&) = delete;
    InputExtensions& operator=(const InputExtensions&) = delete;

    void negotiate(const std::vector<XrExtensionProperties>& available, std::vector<const char*>& enabled);
    using CollectBinding = std::function<void(const std::string&, XrActionSuggestedBinding)>;
    void createActions(XrInstance instance, XrActionSet set, XrSession session,
                       const CollectBinding& collect);
    void profileResult(XrPath profile, bool accepted);
    void sync(XrSession session, XrSpace space, XrTime time, bool focused) noexcept;
    void releaseSession() noexcept;

private:
    struct Channel {
        size_t actionIndex = 0;
        InputType type = InputType::Boolean;
        XrAction action = XR_NULL_HANDLE;
        XrPath user = XR_NULL_PATH;
        std::string_view userPath;
        XrSpace space = XR_NULL_HANDLE;
    };
    struct Provider {
        std::shared_ptr<InputExtension> target;
        InputExtensionDescription description;
        bool enabled = false;
        bool bound = false;
        std::vector<XrPath> profiles;
        std::vector<XrPath> acceptedProfiles;
        std::vector<XrAction> actions;
        std::vector<Channel> channels;
        std::vector<InputValue> values;
    };
    static void validate(const InputExtensionDescription& description);
    void createAction(Provider& provider, size_t providerIndex, size_t actionIndex, XrInstance instance,
                      XrActionSet set, XrSession session, const CollectBinding& collect);
    static InputValue read(const Channel& channel, XrSession session, XrSpace space, XrTime time) noexcept;
    std::vector<Provider> providers_;
    bool sessionInitialized_ = false;
};

} // namespace saida::xr
