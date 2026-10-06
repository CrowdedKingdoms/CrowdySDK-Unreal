// Your server code for the Server Object type ck_exec_fixture. The generator writes this file once and never again.
use crate::*;
use ckx_sdk::prelude::*;

impl Functions for CrowdyExecTestBossState {
    fn echo_scalars(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestScalars) -> Result<CrowdyExecTestScalars> {
        Err(Error::new("echo_scalars is not written yet"))
    }

    fn echo_containers(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestContainers) -> Result<CrowdyExecTestContainers> {
        Err(Error::new("echo_containers is not written yet"))
    }

    fn echo_engine(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestEngine) -> Result<CrowdyExecTestEngine> {
        Err(Error::new("echo_engine is not written yet"))
    }

    fn echo_extras(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestExtras) -> Result<CrowdyExecTestExtras> {
        Err(Error::new("echo_extras is not written yet"))
    }

    fn echo_keys(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestKeys) -> Result<CrowdyExecTestKeys> {
        Err(Error::new("echo_keys is not written yet"))
    }

    fn echo_raw(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestRaw) -> Result<CrowdyExecTestRaw> {
        Err(Error::new("echo_raw is not written yet"))
    }

    fn defaults(&mut self, ctx: &Ctx, call: &Call<'_>) -> Result<CrowdyExecTestScalars> {
        Err(Error::new("defaults is not written yet"))
    }

    fn hit(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestHit) -> Result<()> {
        Err(Error::new("hit is not written yet"))
    }

    fn heal(&mut self, ctx: &Ctx, call: &Call<'_>) -> Result<()> {
        Err(Error::new("heal is not written yet"))
    }
}
