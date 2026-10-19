#include "Test.h"
#include "Pattern.h"

#include "dsp/filtering_functions_f16.h"

class DECIMF16:public Client::Suite
    {
        public:
            DECIMF16(Testing::testID_t id);
            virtual void setUp(Testing::testID_t,std::vector<Testing::param_t>& params,Client::PatternMgr *mgr);
            virtual void tearDown(Testing::testID_t,Client::PatternMgr *mgr);
        private:
            #include "DECIMF16_decl.h"
            Client::Pattern<float16_t> coefs;
            Client::Pattern<float16_t> samples;

            Client::LocalPattern<float16_t> output;
            Client::LocalPattern<float16_t> state;

            int nbTaps;
            int nbSamples;
            int decimationFactor;

            arm_fir_decimate_instance_f16  instDecim;
            
            const float16_t *pSrc;
            float16_t *pDst;
            
    };
