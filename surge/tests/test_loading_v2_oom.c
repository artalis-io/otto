/* Fail each V2-owned allocation independently; rejected updates must preserve
 * the previously committed plan and owned evidence. Linked native routines
 * keep their own tested allocators; this targets the new publication boundary. */
#define main loading_v2_other_main
#include "test_loading_v2.c"
#undef main

static size_t fail_at, allocation;
static void *fault_malloc(size_t n) {
    if (fail_at && ++allocation == fail_at) return NULL;
    return malloc(n);
}
static void *fault_calloc(size_t n, size_t size) {
    if (fail_at && ++allocation == fail_at) return NULL;
    return calloc(n, size);
}
#define malloc fault_malloc
#define calloc fault_calloc
#include "../src/sg_loading_v2.c"
#undef malloc
#undef calloc

int main(void) {
    SGContext *ctx = base();configure(ctx);
    uint32_t task=0;SGPlanRoute plan={0,&task,1};
    SGLoadingV2Report *original=NULL;
    assert(sg_loading_v2_validate_plan(ctx,1,&plan,NULL,SG_LOADING_V2_FIND_WITNESSES,NULL,0,NULL,&original)==SG_STATUS_OK);
    view(original);
    uint32_t failures=0;
    for (size_t at=1;at<48;at++) {
        allocation=0;fail_at=at;
        SGLoadingV2Report *result=NULL;
        SGStatus status=sg_loading_v2_validate_plan(ctx,1,&plan,NULL,SG_LOADING_V2_FIND_WITNESSES,NULL,0,NULL,&result);
        fail_at=0;
        if(status!=SG_STATUS_OK) {
            assert(status==SG_STATUS_OUT_OF_MEMORY);
            assert(result==NULL);failures++;
        } else view(result);
        assert(sg_get_unassigned(ctx)==0);
        SGLoadingV2Report *current=NULL;
        assert(sg_loading_v2_solution_report(ctx,&current)==SG_STATUS_OK);
        view(current);view(original);
        sg_loading_v2_report_free(current);sg_loading_v2_report_free(result);
    }
    assert(failures>=5);
    sg_free(ctx);view(original);sg_loading_v2_report_free(original);
    printf("loading v2 OOM: 47 allocation failpoints, %u rejected publications preserved prior snapshot\n",failures);
    return 0;
}
