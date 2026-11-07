// SPDX-License-Identifier: Apache-2.0
#include "domain/protocol/session.hpp"
#include "domain/protocol/wire.hpp"
#include <pdo_schema.hpp>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace ecat;
#define Check(value) do {if(!(value)){std::cerr<<"contract failed at line "<<__LINE__<<"\n";std::abort();}} while(0)
int main(){
    Configuration c;c.can_rx[0]=1;c.uart[0].rx_bytes=8;c.imu=true;
    Limits limits{1024,1024,8192,1,0,1,true};Layout layout;
    Check(BuildLayout(c,limits,layout)==Error::None);
    ConfigurationTransaction transaction;
    for(unsigned sub=1;sub<=22;++sub){
        uint32_t value=0;
        if(sub==1)value=layout.signature;
        if(sub==2)value=c.cycle_us;
        if(sub==3)value=c.watchdog_us;
        if(sub==6)value=1;
        if(sub==11)value=8;
        if(sub==22)value=1;
        Check(transaction.Write(sub,value)==Error::None);
    }
    Protection protection;protection.installed=true;protection.stop=[](void*,Error){};
    Session session(7,protection);Check(session.Configure(transaction,limits,layout.rx,layout.tx)==Error::None);
    std::array<uint8_t,1024> rx{},tx{};Actions actions;
    Put32(rx.data(),42);Put32(rx.data()+4,7);Put32(rx.data()+8,1);rx[12]=kArm;Put16(rx.data()+16,4);
    Check(session.Process({rx.data(),layout.rx.bytes},1000,true,actions)==Error::None);
    Check(session.state()==BusinessState::Active);
    const uint8_t payload[4]={1,2,3,4};Check(session.QueueUart(0,{payload,4},1000,1000));
    Feedback slow;slow.imu_valid=true;slow.imu_sequence=9;slow.imu_capture_us=1000;slow.temperature=25;slow.imu_raw.fill(0x5a);
    Check(session.Publish({tx.data(),layout.tx.bytes},1100,slow,true)==Error::None);
    const auto original=tx;
    session.CompleteStatus(4,0,{11,22,33});
    Feedback fast;
    Check(session.Publish({tx.data(),layout.tx.bytes},1200,fast,false)==Error::None);
    const auto uart=layout.uart_tx[0];
    Check(std::equal(original.begin()+uart,original.begin()+uart+pdo::tx::uart::header_size+8,tx.begin()+uart));
    Check(std::equal(original.begin()+layout.imu,original.begin()+layout.imu+pdo::tx::imu::size,tx.begin()+layout.imu));
    Check(U32(tx.data()+layout.diagnostics+8)==4);
    Check(U32(tx.data()+8)==U32(original.data()+8)+1);
    session.Protect(Error::Transport);
    Check(session.Publish({tx.data(),layout.tx.bytes},1300,fast,false)==Error::None);
    Check(tx[20]==uint8_t(BusinessState::Protected));
    Check((tx[uart+pdo::tx::uart::flags]&1)==0);
    Check(tx[layout.imu+pdo::tx::imu::flags]==0);
    session.Reset(8,protection);
    Check(session.Configure(transaction,limits,layout.rx,layout.tx)==Error::None);
    tx=original; // Deliberately retain an old-session buffer across rearm.
    Check(session.Publish({tx.data(),layout.tx.bytes},1400,fast,false)==Error::None);
    Check(U32(tx.data())==8 && tx[20]==uint8_t(BusinessState::Ready));
    Check(tx[layout.imu+pdo::tx::imu::flags]==0);
    Check((tx[uart+pdo::tx::uart::flags]&1)==0);
    std::cout<<"PASS: fast publication preserves UART/IMU; status advances; protection/rearm clear validity\n";
}
