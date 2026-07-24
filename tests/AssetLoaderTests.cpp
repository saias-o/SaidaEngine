#include "project/AssetLoader.hpp"
#include "project/AssetRegistry.hpp"
#include "scene/animation/AnimationAssetDecoders.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

bool waitForTerminal(saida::AssetHandle& handle) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (handle.state() == saida::AssetLoadState::Queued ||
           handle.state() == saida::AssetLoadState::Loading) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// Memory accounting is released by Entry's destructor; the worker can hold
// its reference briefly after the terminal state, so we poll instead of
// asserting instantly.
bool waitForResidentZero(saida::AssetLoader& loader) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        loader.collectGarbage();
        if (loader.stats().residentBytes == 0) return true;
        std::this_thread::yield();
    }
    return false;
}

void writeBytes(const std::filesystem::path& path, size_t count, uint8_t value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    std::string bytes(count, static_cast<char>(value));
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "SaidaAssetLoaderTests";
    fs::remove_all(root);
    writeBytes(root / "assets/small.bin", 4096, 0x2a);
    writeBytes(root / "assets/too-large.bin", 32768, 0x7f);
    writeText(root / "assets/hero.srig",
              R"({"schema":1,"name":"Hero","semantics":{"hips":"Hips"},"height":1.8})");
    writeText(root / "assets/run.sclip",
              R"({"schema":1,"source":"hero.glb#Run","name":"Run","loop":true,"speed":1})");
    writeText(root / "assets/locomotion.sgraph",
              R"({"schema":2,"name":"Locomotion","parameters":[{"name":"speed","type":"float","default":0}],"clips":{"idle":"hero.glb#Idle"},"states":[{"name":"Idle","play":"idle","loop":true}],"initial":"Idle","transitions":[]})");
    writeText(root / "assets/bad.sgraph",
              R"({"schema":2,"name":"Bad","clips":{},"states":[{"name":"Idle","play":"missing"}],"initial":"Idle"})");

    saida::AssetRegistry registry;
    assert(registry.load(root.string()));
    const saida::AssetID small = registry.registerAsset("assets/small.bin", saida::AssetType::Unknown);
    const saida::AssetID large = registry.registerAsset("assets/too-large.bin", saida::AssetType::Unknown);
    const saida::AssetID missing = registry.registerAsset("assets/missing.bin", saida::AssetType::Unknown);

    saida::AssetLoader loader(&registry, 8192);
    const auto requestStart = std::chrono::steady_clock::now();
    auto handle = loader.request(small, saida::AssetLoadPriority::Critical);
    const auto requestTime = std::chrono::steady_clock::now() - requestStart;
    assert(requestTime < std::chrono::milliseconds(50));
    assert(waitForTerminal(handle));
    assert(handle.ready());
    assert(handle.bytes().size() == 4096);
    assert(handle.bytes().front() == 0x2a);

    auto shared = loader.request(small);
    assert(shared.ready());
    assert(shared.id() == handle.id());
    assert(handle.referenceCount() >= 2);

    auto overBudget = loader.request(large);
    assert(waitForTerminal(overBudget));
    assert(overBudget.failed());
    assert(overBudget.error().find("budget") != std::string::npos);

    auto absent = loader.request(missing);
    assert(waitForTerminal(absent));
    assert(absent.failed());
    assert(absent.error().find("not found") != std::string::npos);

    handle.reset();
    shared.reset();
    overBudget.reset();
    absent.reset();
    assert(waitForResidentZero(loader));

    for (int i = 0; i < 32; ++i) {
        auto cycle = loader.request(small);
        assert(waitForTerminal(cycle));
        assert(cycle.ready());
        cycle.reset();
        assert(waitForResidentZero(loader));
    }

    // Decoding failures must stay explicit and deterministic.

    // A decoder consumes the raw bytes and produces a typed payload; the
    // accounting switches to the decoded size.
    struct Sum { uint64_t total = 0; };
    auto sumDecoder = [](std::vector<uint8_t>&& bytes, saida::AssetDecodeResult& out,
                         std::string&) {
        auto sum = std::make_shared<Sum>();
        for (uint8_t b : bytes) sum->total += b;
        out.payload = sum;
        out.bytes = sizeof(Sum);
        return true;
    };
    auto decoded = loader.request(small, saida::AssetLoadPriority::Normal,
                                  saida::AssetPayloadKind::Image, sumDecoder);
    assert(waitForTerminal(decoded));
    assert(decoded.ready());
    assert(decoded.bytes().empty());  // raw bytes freed after decoding
    auto sum = std::static_pointer_cast<Sum>(decoded.payload());
    assert(sum && sum->total == 4096ull * 0x2a);
    assert(loader.stats().residentBytes == sizeof(Sum));

    // A Raw request for the same id does not share the decoded entry.
    auto rawAgain = loader.request(small);
    assert(waitForTerminal(rawAgain));
    assert(rawAgain.ready());
    assert(rawAgain.bytes().size() == 4096);
    assert(rawAgain.payload() == nullptr);

    decoded.reset();
    rawAgain.reset();
    assert(waitForResidentZero(loader));

    // A decoder that fails produces a Failed state with its diagnostic, and
    // leaves nothing resident.
    auto failingDecoder = [](std::vector<uint8_t>&&, saida::AssetDecodeResult&,
                             std::string& error) {
        error = "corrupt payload";
        return false;
    };
    auto failed = loader.request(small, saida::AssetLoadPriority::Normal,
                                 saida::AssetPayloadKind::MeshObj, failingDecoder);
    assert(waitForTerminal(failed));
    assert(failed.failed());
    assert(failed.error().find("corrupt payload") != std::string::npos);
    failed.reset();
    assert(waitForResidentZero(loader));

    // The three standalone animation formats use the same pipeline:
    // request() stays immediate, the JSON parsing happens in the decoder, and
    // the typed payload is only visible at Ready.
    const auto requestAnimation = [&](const char* path, saida::AssetType type,
                                      saida::AssetPayloadKind kind,
                                      saida::AssetDecoder decoder) {
        const auto start = std::chrono::steady_clock::now();
        auto result = loader.request(path, type, saida::AssetLoadPriority::High, kind,
                                     std::move(decoder));
        assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50));
        return result;
    };

    auto rigAsset = requestAnimation("assets/hero.srig", saida::AssetType::Rig,
                                     saida::AssetPayloadKind::RigAsset,
                                     saida::makeRigAssetDecoder());
    auto clipView = requestAnimation("assets/run.sclip", saida::AssetType::Animation,
                                     saida::AssetPayloadKind::ClipView,
                                     saida::makeClipViewDecoder());
    auto animGraph = requestAnimation("assets/locomotion.sgraph",
                                      saida::AssetType::Animation,
                                      saida::AssetPayloadKind::AnimGraph,
                                      saida::makeAnimGraphDecoder());
    assert(waitForTerminal(rigAsset));
    assert(waitForTerminal(clipView));
    assert(waitForTerminal(animGraph));
    assert(rigAsset.ready() && clipView.ready() && animGraph.ready());
    auto decodedRig = std::static_pointer_cast<saida::DecodedRigAsset>(rigAsset.payload());
    auto decodedView = std::static_pointer_cast<saida::DecodedClipView>(clipView.payload());
    auto decodedGraph = std::static_pointer_cast<saida::DecodedAnimGraph>(animGraph.payload());
    assert(decodedRig && decodedRig->asset.name == "Hero");
    assert(decodedView && decodedView->view.name == "Run");
    assert(decodedGraph && decodedGraph->graph.initial == "Idle");

    rigAsset.reset();
    clipView.reset();
    animGraph.reset();
    assert(waitForResidentZero(loader));

    auto badGraph = requestAnimation("assets/bad.sgraph", saida::AssetType::Animation,
                                     saida::AssetPayloadKind::AnimGraph,
                                     saida::makeAnimGraphDecoder());
    assert(waitForTerminal(badGraph));
    assert(badGraph.failed());
    assert(badGraph.error().find("missing") != std::string::npos);
    badGraph.reset();
    assert(waitForResidentZero(loader));

    fs::remove_all(root);
    return 0;
}
