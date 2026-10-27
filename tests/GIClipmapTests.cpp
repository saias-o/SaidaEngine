#include "render/gi/GIClipmap.hpp"
#include <cassert>
#include <iostream>
#include <set>
#include <cmath>

namespace {
void maximumQuality() {
    using namespace saida;
    GIConfig c;c.follow=true;c.cascades=6;c.voxels={128,64,128};
    c.updateAll=true;c.catchUp=1;
    c.probeBudget=c.probeCount();c.brickBudget=c.bricksPerCascade()*c.cascades;
    GIClipmap field(c);
    // Initial fill, steady motion, reversal, and teleport must all finish
    // their entering slabs in the very frame whose probes consume them.
    for(const glm::dvec3 camera:{glm::dvec3(0),glm::dvec3(2.4,0,0),glm::dvec3(-4,2,8),glm::dvec3(1000)}) {
        field.move(camera,1./60.);field.plan();
        assert(field.queuedBricks()==0);
        assert(field.probes().size()==size_t(c.probeCount()));
        assert(field.warmupProgress()==1.f);
        std::set<int> probes;
        for(auto p:field.probes())assert(probes.insert(p.x).second);
        field.invalidate(camera-glm::dvec3(10000),camera+glm::dvec3(10000));
        field.plan();
        assert(field.bricks().size()==size_t(c.brickBudget));
        std::set<uint32_t> bricks(field.bricks().begin(),field.bricks().end());
        assert(bricks.size()==field.bricks().size());
        assert(field.queuedBricks()==0);
    }
}

void enteringSlabs() {
    using namespace saida;
    GIConfig c;c.follow=true;c.spacing=2;c.brickBudget=8;c.probeBudget=64;
    GIClipmap field(c);
    for(int f=0;f<100;++f) {field.move({0,0,0});field.plan();}
    assert(field.warmupProgress()==1.f);
    // A full maintenance queue must not put entering storage behind old edits.
    field.invalidate({-100,-100,-100},{100,100,100});
    field.move({2.2,0,0});
    const std::set<uint32_t> entering(field.invalidProbes().begin(),field.invalidProbes().end());
    assert(entering.size()==size_t(c.probes.y*c.probes.z));
    for(int f=0;f<4;++f) {
        field.plan();
        assert(field.bricks().size()==size_t(c.brickBudget));
        for(auto b:field.bricks()) assert(GIClipmap::coord(int(b),c.bricks()).x==4);
        if(f<3) assert(field.probes().empty());
    }
    assert(!field.probes().empty());
    for(auto p:field.probes()) {
        if(entering.count(uint32_t(p.x))) assert(p.z==1);
        else assert(p.z==0);
    }
    // The reset jobs precede edits even when their physical indices sort last.
    assert(field.probes().size()>=entering.size());
    for(size_t i=0;i<entering.size();++i) assert(entering.count(uint32_t(field.probes()[i].x)));
    // Ordinary streaming edits never block refreshes behind a nonempty queue.
    for(int f=0;f<30;++f) {
        field.move({2.2,0,0});
        field.invalidate({-100,-100,-100},{100,100,100});field.plan();
        assert(!field.probes().empty());
        assert(field.queuedBricks()>0);
    }
}

void anticipation() {
    using namespace saida;
    for(double hz:{30.,60.,120.}) {
        GIConfig c;c.cascades=5;c.follow=true;c.spacing=2;c.brickBudget=32;c.probeBudget=128;
        GIClipmap field(c);
        glm::dvec3 camera(1000000000.,120.,-1000000000.);
        for(int f=0;f<int(hz*3);++f) {
            field.move(camera,1./hz);field.plan();camera.x+=6./hz;
        }
        const auto fine=field.cascade(0);
        assert(field.warmupProgress()>.95f);
        const double lead=fine.origin.x+c.probes.x*fine.spacing*.5-camera.x;
        assert(lead>2. && lead<=6.); // about .75 seconds ahead of a walking eye
        const auto coarse=field.cascade(4);
        assert(coarse.origin.x+c.probes.x*coarse.spacing*.5>=camera.x-coarse.spacing);
        // Stopping and reversing keep the eye inside ready retained coverage.
        for(int f=0;f<int(hz);++f) {field.move(camera,1./hz);field.plan();}
        const double stopped=field.cascade(0).origin.x+16.-camera.x;
        assert(std::abs(stopped)<=2.);
        for(int f=0;f<int(hz*3);++f) {
            camera+=glm::dvec3(-90.,30.,-60.)/hz;
            field.move(camera,1./hz);field.plan();
            for(int k=0;k<c.cascades;++k) {
                const auto& cascade=field.cascade(k);
                const auto local=(camera-cascade.origin)/cascade.spacing;
                assert(glm::all(glm::greaterThanEqual(local,glm::dvec3(1))));
                assert(glm::all(glm::lessThan(local,glm::dvec3(c.probes)-glm::dvec3(1))));
            }
            assert(field.probes().size()<=size_t(c.maxProbeBudget()));
            assert(field.bricks().size()<=size_t(c.brickBudget));
        }
        // A teleport resets prediction instead of projecting its huge velocity.
        field.move({0,0,0},1./hz);
        assert(field.cascade(0).origin==glm::dvec3(-16,-8,-16));
    }
}
}

int main() {
    using namespace saida;
    enteringSlabs();anticipation();maximumQuality();
    GIConfig c; c.cascades=6; c.follow=true; c.brickBudget=24; c.probeBudget=256; c.spacing=2;
    GIClipmap field(c);
    field.move({0,0,0});
    assert(field.invalidProbes().size()==size_t(c.probeCount()));
    for(int frame=0;frame<240;++frame) {
        field.move({0,0,0}); field.plan();
        // Catching up while probes wait for their first trace, the normal budget once warm.
        assert(field.probes().size()<=size_t(c.maxProbeBudget()));
        assert(field.bricks().size()<=size_t(c.brickBudget));
        std::set<int> unique;
        for(auto job:field.probes()) assert(unique.insert(job.x).second);
    }
    assert(field.warmupProgress()==1.f);
    field.move({0,0,0}); field.plan();
    assert(field.probes().size()<=size_t(c.probeBudget));
    const auto old=field.cascade(0);
    field.move({2.2,0,0});
    assert(field.cascade(0).cell.x==old.cell.x+1);
    assert(field.invalidProbes().size()==size_t(c.probes.y*c.probes.z));
    // A retained absolute cell keeps its physical index on both sides of zero.
    const glm::i64vec3 absolute(-3,2,-6);
    const auto physical=GIClipmap::wrap(absolute,c.probes);
    assert(physical.x==13 && physical.z==10);
    const auto local=absolute-field.cascade(0).cell;
    assert(GIClipmap::wrap(local+glm::i64vec3(field.cascade(0).probeWrap),c.probes)==physical);
    field.move({2.01,0,0});
    assert(field.invalidProbes().empty());
    field.move({500,0,0});
    assert(field.warmupProgress()<1.f);
    for(int frame=0;frame<240;++frame) { field.move({500,0,0});field.plan(); }
    assert(field.warmupProgress()==1.f);
    GIConfig anchored; GIClipmap room(anchored);
    room.move({500,0,0}); const auto origin=room.cascade(0).origin;
    for(int f=0;f<80;++f) {room.move({f*100.,0,0},1./60.);room.plan();}
    assert(room.cascade(0).origin==origin && room.warmupProgress()==1.f);
    std::cout << "PASS GI clipmaps: anticipation at 30/60/120 Hz, reversal and teleport, entering slab readiness and priority, streaming fairness, bounded budgets, toroidal retention, anchored volume\n";
}
