/* faults_empty.c - page faults of an empty program until main (comparison for the startup of rosetta) */
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>

int main(void)
{
    task_events_info_data_t ev;
    mach_msg_type_number_t n = TASK_EVENTS_INFO_COUNT;
    memset(&ev, 0, sizeof ev);
    task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev, &n);
    printf("%d page faults until main (%d on copy)\n", ev.faults, ev.cow_faults);
    return 0;
}
