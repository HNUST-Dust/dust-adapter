// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <csignal>
#include <cmath>
#include <thread>
#include <stdexcept>
#include "chassis/chassis_controller.h"
#include "hardware/motors/motor_protocol.hpp"
#include "hardware/remote/wfly_sbus_protocol.h"
#include "common/device_pdo.hpp"

namespace live_test {
namespace dev = dust_control::devices;
namespace ch = dust_control::chassis;
namespace wire = dust::device_pdo;
inline std::atomic<bool> arm_request{false}, stop_request{false};
static_assert(std::atomic<bool>::is_always_lock_free);
inline void Signal(int signal) {
    if(signal == SIGUSR1) arm_request.store(true, std::memory_order_relaxed);
    else stop_request.store(true, std::memory_order_relaxed);
}
inline uint64_t Clock() { timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec; }
struct Decision { std::array<wire::Payload,6> payloads{};uint64_t generated_ns=0;bool energized=false;bool nonzero=false; };
struct Summary {
    ch::ChassisCycleInput input{};
    std::array<dev::MotorFeedback,6> motors{};
    uint64_t ns=0;
    unsigned state=0;
    bool disabled_seen=false,armed=false,neutral=false;
};
class Bridge {
 public:
    Bridge(bool live,const std::string& config)
      : live_(live), motors_(dev::LoadMotorConfig(config+"/motors_params.yaml")),
        config_(ch::LoadChassisConfig(config+"/chassis_params.yaml")),
        decoder_(motors_), encoder_(motors_),
        controller_(Deadlines(),motors_.wheel_torque_limit_nm(),config_) {
        dev::ValidateMotorAdapterConfig(motors_);
        for(const auto& route:wire::kRoutes) {
            bool found=false;
            for(unsigned i=0;i<6;++i) found |= motors_.present[i] && motors_.addresses[i].bus==route.bus &&
                motors_.addresses[i].command_id==route.command && motors_.addresses[i].feedback_id==route.feedback;
            if(!found) throw std::runtime_error("Motor YAML does not match DTG1 routes");
        }
        times_.reserve(65000);wake_.reserve(65000);
        signal(SIGUSR1,Signal);signal(SIGINT,Signal);signal(SIGTERM,Signal);
    }
    ~Bridge(){ Finish(); }
    void FeedMotor(wire::Device device,const wire::Payload& bytes,uint64_t us){
        const auto* route=wire::Route(device);decoder_.Consume(route->bus,route->feedback,bytes,us,true);
    }
    void FeedImu(const uint8_t* bytes,uint32_t sequence,uint64_t us){
        dust::hi91::Compact compact{};std::copy_n(bytes,compact.size(),compact.begin());
        const auto frame=dust::hi91::ExpandControlFields(compact,sequence);imu_.Consume(frame,us);
    }
    void FeedRemote(const uint8_t* bytes,size_t length,bool gap,uint64_t us){
        if(gap)remote_.Discontinuity();remote_.Consume({bytes,length},us);
    }
    void PollRemote(uint64_t us){remote_.PollTimeout(us);}
    void Start(){if(!started_.exchange(true))worker_=std::thread([this]{Run();});}
    bool ShouldStop()const{return stop_request.load()||fault_.load()!=0||finished_.load();}
    bool Payloads(std::array<wire::Payload,6>& payloads){
        const auto d=decision_.Read();
        if(!d.energized)return false;
        if(Clock()<d.generated_ns || Clock()-d.generated_ns>2000000){fault_=7;return false;}
        payloads=d.payloads;++selected_batches_;selected_nonzero_batches_+=d.nonzero;return true;
    }
    int Fault()const{return fault_.load();}
    void Finish(){exit_=true;if(worker_.joinable())worker_.join();}
    void Print(){
        Finish();const auto s=summary_.Read();
        auto stats=[](std::vector<uint64_t>& a){
            std::sort(a.begin(),a.end());uint64_t sum=0;for(auto n:a)sum+=n;
            const auto at=[&](size_t p){return a.empty()?0.0:a[(a.size()*p+99)/100-1]/1000.0;};
            printf("{\"count\":%zu,\"mean_us\":%.2f,\"p50_us\":%.2f,\"p99_us\":%.2f,\"max_us\":%.2f}",a.size(),a.empty()?0:double(sum)/a.size()/1000,at(50),at(99),a.empty()?0:a.back()/1000.0);
        };
        printf(",\"controller\":{\"live_requested\":%s,\"fault\":%d,\"ticks\":%u,\"feedback_valid_ticks\":%u,\"ready_ticks\":%u,\"missed_releases\":%u,\"deadline_overruns\":%u,\"energized_ticks\":%u,\"max_abs_joint_target_nm\":%.6f,\"max_abs_wheel_target_nm\":%.6f,\"joint_limit_nm\":%.3f,\"wheel_limit_nm\":%.3f,\"compute\":",live_?"true":"false",Fault(),ticks_,valid_ticks_,ready_ticks_,missed_,overruns_,energized_ticks_,max_joint_,max_wheel_,config_.joint_torque_limit,motors_.wheel_torque_limit_nm());
        stats(times_);printf(",\"selected_control_batches\":%u,\"selected_nonzero_batches\":%u",selected_batches_,selected_nonzero_batches_);printf(",\"wake_lateness\":");stats(wake_);
        printf(",\"state_ticks\":[");for(unsigned i=0;i<states_.size();++i)printf("%s%u",i?",":"",states_[i]);
        printf("],\"last\":{\"state\":%u,\"disabled_seen\":%s,\"operator_armed\":%s,\"remote_sequence\":%u,\"remote_enable\":%s,\"neutral\":%s,\"remote_fresh\":%s,\"imu_fresh\":%s,\"feedback_valid\":%s,\"feedback_invalid_mask\":%u,\"motor_stale_mask\":%u,\"pitch_deg\":%.6f,\"roll_deg\":%.6f,\"leg_length_m\":[%.6f,%.6f],\"joint_position_rad\":[%.6f,%.6f,%.6f,%.6f],\"dm_enabled\":[%s,%s,%s,%s]}}",s.state,s.disabled_seen?"true":"false",s.armed?"true":"false",s.input.remote.sequence,s.input.remote.robot_enable?"true":"false",s.neutral?"true":"false",s.input.remote_fresh?"true":"false",s.input.imu_fresh?"true":"false",s.input.feedback_valid?"true":"false",s.input.feedback_invalid_mask,s.input.motor_stale_mask,s.input.imu.pitch_deg,s.input.imu.roll_deg,s.input.leg.left.length,s.input.leg.right.length,s.motors[2].position_rad,s.motors[3].position_rad,s.motors[4].position_rad,s.motors[5].position_rad,s.motors[2].enabled?"true":"false",s.motors[3].enabled?"true":"false",s.motors[4].enabled?"true":"false",s.motors[5].enabled?"true":"false");
    }
 private:
    static ch::InputDeadlines Deadlines(){ch::InputDeadlines d;d.remote_us=100000;return d;}
    void Run(){try{
        cpu_set_t cpu;CPU_ZERO(&cpu);CPU_SET(2,&cpu);sched_param p{70};
        if(pthread_setaffinity_np(pthread_self(),sizeof(cpu),&cpu)||pthread_setschedparam(pthread_self(),SCHED_FIFO,&p)){fault_=1;finished_=true;return;}
        pthread_setname_np(pthread_self(),"dtg1-control");
        uint64_t release=Clock(),enabled_at=0;bool disabled_seen=false,armed=false,was_enabled=false;
        while(!exit_ && !stop_request && !fault_){
            release+=1000000;timespec t{time_t(release/1000000000),long(release%1000000000)};
            int rc;do{rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,nullptr);}while(rc==EINTR);
            if(rc){fault_=2;break;}
            const auto start=Clock();const auto late=start-release;
            if(late>=1000000){missed_+=late/1000000;release+=(late/1000000)*1000000;}
            if(late>=20000000){fault_=3;break;}
            ch::ControllerInputs input{};input.motors=decoder_.Read();const auto remote=remote_.Read();
            input.remote=remote.remote;input.remote_disable_pose=remote.disable_pose;input.imu=imu_.Read();
            disabled_seen |= remote.disable_pose;
            // Start from neutral; live trial permits translation and rotation, not action modes.
            const bool neutral=std::abs(input.remote.chassis_x)<=.05 && std::abs(input.remote.chassis_rotate)<=.05 && std::abs(input.remote.leg_length_delta)<=.05;
            if(arm_request.exchange(false)){
                const auto last=summary_.Read();
                if(!live_||!disabled_seen||input.remote.robot_enable||!last.input.feedback_valid||!last.input.remote_fresh||!neutral||std::abs(input.imu.pitch_deg)>20||!std::isfinite(input.imu.pitch_deg)||!std::isfinite(input.imu.roll_deg)||std::abs(input.imu.roll_deg)>20){fault_=4;break;}
                armed=true;
            }
            if(armed && (input.remote.jump||input.remote.climb_stairs||std::abs(input.remote.leg_length_delta)>.05)){fault_=5;break;}
            ch::ChassisCycleInput observed{};
            const auto output=controller_.Tick(input,start/1000,live_&&armed&&disabled_seen,&observed);
            const auto state=controller_.state();
            if(armed && input.remote.robot_enable){if(!enabled_at)enabled_at=start;was_enabled=true;}
            if(armed && ((enabled_at && start-enabled_at>=30000000000ULL)||(was_enabled&&!input.remote.robot_enable))){finished_=true;break;}
            if(armed && (!observed.feedback_valid||!observed.remote_fresh||state==ch::ChassisControlState::kSafetyStop||state==ch::ChassisControlState::kActionFault||state==ch::ChassisControlState::kTiltFault)){summary_.Publish({observed,input.motors,start,unsigned(state),disabled_seen,armed,neutral});fault_=6;break;}
            const bool energized=live_&&armed&&observed.requested_enable;
            auto targets=output.motor_targets;
            if(!energized){for(unsigned i=0;i<6;++i)targets[i]={i<2?dev::MotorMode::kTorque:dev::MotorMode::kDisable,0};}
            else for(auto& target:targets)if(target.mode==dev::MotorMode::kNone)target={dev::MotorMode::kTorque,0};
            for(unsigned i=0;i<6;++i){
                if(!std::isfinite(targets[i].torque_nm)){fault_=8;break;}
                if(i<2)max_wheel_=std::max(max_wheel_,std::abs(targets[i].torque_nm));else max_joint_=std::max(max_joint_,std::abs(targets[i].torque_nm));
            }
            if(fault_)break;
            const auto encoded=encoder_.Encode(targets);if(!encoded||encoded->count!=6){fault_=9;break;}
            Decision decision;decision.generated_ns=Clock();decision.energized=energized;
            for(const auto& target:targets)decision.nonzero |= target.mode==dev::MotorMode::kTorque && std::abs(target.torque_nm)>0.000001;
            for(const auto& frame:std::span(encoded->frames.data(),encoded->count)){
                auto found=std::find_if(wire::kRoutes.begin(),wire::kRoutes.end(),[&](const auto& r){return r.bus==frame.bus&&r.command==frame.id;});
                if(found==wire::kRoutes.end()){fault_=10;break;}
                decision.payloads[found-wire::kRoutes.begin()]=frame.bytes;
            }
            if(fault_)break;
            decision_.Publish(decision);summary_.Publish({observed,input.motors,start,unsigned(state),disabled_seen,armed,neutral});
            const auto finish=Clock();++ticks_;valid_ticks_+=observed.feedback_valid;
            ready_ticks_+=observed.feedback_valid&&observed.remote_fresh&&disabled_seen&&neutral;
            energized_ticks_+=energized;++states_[unsigned(state)];overruns_+=finish-release>=1000000;
            if(times_.size()<times_.capacity())times_.push_back(finish-start);
            if(wake_.size()<wake_.capacity())wake_.push_back(late);
        }
    }catch(...){fault_=11;}finished_=true;}
    bool live_;dev::MotorConfig motors_;ch::ChassisConfig config_;
    dev::MotorFeedbackDecoder decoder_;dev::MotorEncoder encoder_;
    dev::Hi91Decoder imu_{true};dev::WflySbusDecoder remote_;ch::ChassisController controller_;
    dust_control::concurrency::PublishedSnapshot<Decision> decision_;
    dust_control::concurrency::PublishedSnapshot<Summary> summary_;
    std::atomic<bool> started_{false},exit_{false},finished_{false};std::atomic<int> fault_{0};
    std::thread worker_;std::vector<uint64_t> times_,wake_;
    std::array<unsigned,11> states_{};unsigned ticks_=0,valid_ticks_=0,ready_ticks_=0,missed_=0,overruns_=0,energized_ticks_=0;
    unsigned selected_batches_=0,selected_nonzero_batches_=0;
    double max_joint_=0,max_wheel_=0;
};
}
