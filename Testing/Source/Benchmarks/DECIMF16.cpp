#include "DECIMF16.h"
#include "Error.h"

   
    void DECIMF16::test_fir_decimate_f16()
    {
       arm_fir_decimate_f16(&instDecim,this->pSrc,this->pDst,this->nbSamples);
    } 
    
    void DECIMF16::setUp(Testing::testID_t id,std::vector<Testing::param_t>& params,Client::PatternMgr *mgr)
    {


       std::vector<Testing::param_t>::iterator it = params.begin();
       this->nbTaps = *it++;
       this->nbSamples = *it++;
       

       samples.reload(DECIMF16::SAMPLES1_F16_ID,mgr,this->nbSamples);
       coefs.reload(DECIMF16::COEFS1_F16_ID,mgr,this->nbTaps);

       output.create(this->nbSamples,DECIMF16::OUT_SAMPLES_F16_ID,mgr);

       switch(id)
       {
           case TEST_FIR_DECIMATE_F16_1:
              this->decimationFactor = *it;

              state.create(this->nbSamples + this->nbTaps - 1,DECIMF16::STATE_F16_ID,mgr);

              arm_fir_decimate_init_f16(&instDecim,
                 this->nbTaps,
                 this->decimationFactor,
                 coefs.ptr(),
                 state.ptr(),
                 this->nbSamples);
           break;

       }

       this->pSrc=samples.ptr();
       this->pDst=output.ptr();
       
    }

    void DECIMF16::tearDown(Testing::testID_t id,Client::PatternMgr *mgr)
    {
    }
