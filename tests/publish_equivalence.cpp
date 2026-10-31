#define main core_contracts_main
#include "ethercat_core_contracts.cpp"
#undef main
int main() {
    for (unsigned mask=0; mask<64; ++mask) {
        auto c=Config();
        for (unsigned i=0;i<6;++i) c.uart[i]={uint16_t((mask&(1U<<i))?8:0),uint16_t((mask&(1U<<i))?8:0)};
        c.can_rx={uint8_t(mask%5),uint8_t((mask/4)%3),0,4}; c.imu=mask&1;
        Fixture f(true,0x12345678U,c);
        f.Header(1); CHECK(f.Process()==Error::None);
        const uint8_t uart_data[]={1,2,3,4,5};
        for (unsigned port=0;port<6;++port)
            if(c.uart[port].rx_bytes) CHECK(f.core.QueueUart(port,{uart_data,5},1000,1000,false));
        Core::CanEvent frames[4]{};
        for (unsigned i=0;i<4;++i) {frames[i].id=0x13+i;frames[i].sequence=i+1;frames[i].length=i+1;frames[i].capture_us=1000;frames[i].data.fill(uint8_t(mask+i));}
        for (unsigned step=0;step<6;++step) {
            for(unsigned bus=0;bus<4;++bus) f.feedback.can[bus]={step,mask,frames,uint16_t(step%2?c.can_rx[bus]:0)};
            f.feedback.imu_valid=c.imu && (step&1); f.feedback.imu_capture_us=1000;
            f.feedback.temperature=25.5F; f.feedback.imu_raw.fill(uint8_t(step));
            if (step%2==0) std::fill(f.tx.begin(),f.tx.end(),uint8_t(0xa5));
            CHECK(f.core.Publish({f.tx.data(),f.tx.size()},1000+step,f.feedback,step%2==0)==Error::None);
            CHECK(std::fwrite(f.tx.data(),1,f.tx.size(),stdout)==f.tx.size());
        }
        f.core.Protect(Error::Transport);
        CHECK(f.core.Publish({f.tx.data(),f.tx.size()},1010,f.feedback,false)==Error::None);
        CHECK(std::fwrite(f.tx.data(),1,f.tx.size(),stdout)==f.tx.size());
    }
}
