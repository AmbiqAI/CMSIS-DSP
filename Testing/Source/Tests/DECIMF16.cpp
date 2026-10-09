#include "DECIMF16.h"
#include <stdio.h>
#include "Error.h"

#define SNR_THRESHOLD 60

/* 

Reference patterns are generated with
a double precision computation.

The outputs include values close to zero, so the error is checked
against an absolute plus a relative bound.

*/
#define ABS_ERROR (2.0e-3)
#define REL_ERROR (2.0e-2)


    void DECIMF16::test_fir_decimate_f16()
    {
        int nbTests;
        int nb;
        uint32_t *pConfig = config.ptr();

        const float16_t * pSrc = input.ptr();
        float16_t * pDst = output.ptr();
        float16_t * pCoefs = coefs.ptr();

        nbTests=config.nbSamples() / 4;

        for(nb=0;nb < nbTests; nb++)
        {

            this->q = pConfig[0];
            this->numTaps = pConfig[1];
            this->blocksize = pConfig[2];
            this->refsize = pConfig[3];


            pConfig += 4;

            this->status=arm_fir_decimate_init_f16(&(this->S),
               this->numTaps,
               this->q,
               pCoefs,
               state.ptr(),
               this->blocksize);



            ASSERT_TRUE(this->status == ARM_MATH_SUCCESS);

            arm_fir_decimate_f16(
                 &(this->S),
                 pSrc,
                 pDst,
                 this->blocksize);

            pSrc += this->blocksize;
            pDst += this->refsize;

            pCoefs += this->numTaps;
        }


        ASSERT_EMPTY_TAIL(output);

        ASSERT_SNR(output,ref,(float32_t)SNR_THRESHOLD);

        ASSERT_CLOSE_ERROR(output,ref,ABS_ERROR,REL_ERROR);

    } 

   
    void DECIMF16::setUp(Testing::testID_t id,std::vector<Testing::param_t>& params,Client::PatternMgr *mgr)
    {
      
       (void)params;
       
       switch(id)
       {
        case DECIMF16::TEST_FIR_DECIMATE_F16_1:
          config.reload(DECIMF16::CONFIGSDECIMF16_ID,mgr);
         
          input.reload(DECIMF16::INPUT1_F16_ID,mgr);
          coefs.reload(DECIMF16::COEFS1_F16_ID,mgr);

          ref.reload(DECIMF16::REF1_DECIM_F16_ID,mgr);
          state.create(16 + 768 - 1,DECIMF16::STATE_F16_ID,mgr);

          break;

       }
      

       

       output.create(ref.nbSamples(),DECIMF16::OUT_F16_ID,mgr);
    }

    void DECIMF16::tearDown(Testing::testID_t id,Client::PatternMgr *mgr)
    {
        (void)id;
        output.dump(mgr);
    }
