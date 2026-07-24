#pragma once

struct JSContext;  // quickjs

namespace saida {

class Behaviour;
class JsContext;

class JsEngineBindings {
public:
    static void installForBehaviour(JsContext& context, Behaviour& behaviour);

    // Forgets any storage.flush() resolvers still in flight for this context
    // (teardown/hot-reload): a late IDBFS callback will never touch a
    // destroyed context. No-op on desktop. Called by ~JsContext.
    static void dropPendingFlushes(JSContext* context);
};

} // namespace saida
