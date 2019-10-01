#include <iostream>
#include <td.hh>

int main()
{
    td::launch([] {
        td::sync s;
        td::submit(s, []() { printf("task \n"); });

        td::submit_n(
            s, [](auto i) { printf("task %d \n", i); }, 15);

        td::wait_for_unpinned(s);

        td::sync s2;
        td::submit_n(
            s2,
            [](auto i) {
                // ...
                printf("heavy task %d \n", i);
            },
            500);

        td::wait_for_unpinned(s2);
    });


    return 0;
}
