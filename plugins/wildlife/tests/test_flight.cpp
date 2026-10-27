#include "saida/wildlife/Wildlife.hpp"
#include <cstdio>
#include <cstdlib>
#include <set>

using namespace saida::wildlife;
static void require(bool value,const char* what) {if(!value){std::fprintf(stderr,"FAIL flight: %s\n",what);std::exit(1);}}
static Kind bird() {
    Kind k;k.flight.enabled=true;k.flight.speed=7;k.flight.low=4;k.flight.high=9;
    k.flight.flyMin=3;k.flight.flyMax=5;k.flight.restMin=2;k.flight.restMax=3;
    k.flight.perchLow=1;k.flight.perchHigh=10;k.flight.groundShare=0;
    k.viewRange=300;k.roam=60;k.fleeDistance=8;k.fleeSpeed=10;k.radius=.15f;
    return k;
}
static void setup(Herds& herd,Kind k,uint64_t seed=17) {
    herd.reset({k},{{{0,0},{0},123}},1,seed);herd.setBudget(1);
    herd.setAllowed([](uint16_t,Vec2 p){return std::abs(p.x)<90&&std::abs(p.y)<90;});
    herd.setFlightGround([](Vec2){return 10.f;});
    herd.setPerches({{{20,0},13,1},{{-20,0},14,2}});
}
int main() {
    Herds herd;setup(herd,bird());Scene scene;scene.observer={100,0};scene.facing={1,0};
    std::set<FlightPhase> phases;
    bool gliding=false,flapping=false;float previousY=0;bool prior=false;
    for(int i=0;i<180*60;++i) {
        herd.update(1.f/60,scene);
        for(const auto& a:herd.animals())if(a.alive&&a.flightInitialized) {
            require(std::isfinite(a.elevation)&&std::isfinite(a.heading),"finite state");
            require(a.elevation>=10,"never under terrain");
            if(prior)require(std::abs(a.elevation-previousY)<.12f,"bounded continuous vertical motion");
            previousY=a.elevation;prior=true;phases.insert(a.flightPhase);
            if(a.flightPhase==FlightPhase::Cruise){gliding|=a.flap<.5f;flapping|=a.flap>.5f;}
            if(a.flightPhase==FlightPhase::Perched) {
                require(a.perch!=0&&a.speed==0,"stationary supported perch");
                require(std::abs(a.elevation-(a.perch==1?13.f:14.f))<1e-4f,"exact branch height");
            }
        }
    }
    require(phases.count(FlightPhase::Takeoff),"takes off");require(phases.count(FlightPhase::Cruise),"cruises");
    require(phases.count(FlightPhase::Approach),"approaches");require(phases.count(FlightPhase::Perched),"lands on actual perch");
    require(gliding&&flapping,"flap and glide spells");
    herd.setFlightActivity(0,0);
    for(int i=0;i<60*60;++i)herd.update(1.f/60,scene);
    const auto roost=herd.animals().front();require(roost.flightPhase==FlightPhase::Perched,"rests when activity falls");
    for(int i=0;i<10*60;++i)herd.update(1.f/60,scene);
    require(distance(herd.animals().front().position,roost.position)<1e-5f,"night roost stays put");
    Herds lost=herd;lost.setPerches({});lost.update(1.f/60,scene);
    require(lost.animals().front().flightPhase!=FlightPhase::Perched,"removed supporting branch causes takeoff");
    require(std::abs(lost.animals().front().elevation-roost.elevation)<.12f,"support loss preserves elevation");
    scene.threats={{{roost.position.x,roost.position.y},1}};
    herd.update(1.f/60,scene);require(herd.animals().front().flightPhase!=FlightPhase::Perched,"fright takes off from roost");
    scene.threats.clear();herd.setPerches({});
    for(int i=0;i<90*60;++i)herd.update(1.f/60,scene);
    require(herd.animals().front().flightPhase==FlightPhase::Ground,"missing branches use valid ground");
    Herds a,b;setup(a,bird());setup(b,bird());
    for(int i=0;i<600;++i){a.update(1.f/60,scene);b.update(1.f/60,scene);}
    require(a.animals().front().position.x==b.animals().front().position.x&&a.animals().front().elevation==b.animals().front().elevation,"seeded flight is reproducible");
    require(a.setMemberEnabled(0,0,false),"bird death disables resident");
    for(int i=0;i<600;++i)a.update(1.f/60,scene);
    require(a.live()==0,"dead bird never respawns");
    Herds barrier;setup(barrier,bird());
    barrier.setFlightClear([](Vec2 from,float fy,Vec2 to,float ty) {
        return !(from.x*to.x<0&&std::min(fy,ty)<18.f);
    });
    bool have=false;Animal last;
    for(int i=0;i<120*60;++i) {
        barrier.update(1.f/60,scene);const auto& current=barrier.animals().front();
        if(current.alive&&current.flightInitialized) {
            if(have)require(!(last.position.x*current.position.x<0&&std::min(last.elevation,current.elevation)<18.f),"flight never crosses blocking geometry");
            last=current;have=true;
        }
    }
    require(have,"obstacle test has a live bird");
    std::puts("PASS saida::wildlife flight: continuous takeoff/cruise/landing, perches, roosts, fright, support loss, seeded replay and death");
}
