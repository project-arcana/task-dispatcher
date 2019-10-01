#include <iostream>
#include <td.hh>

int main()
{
    td::launch([] {
        auto s1 = td::submit([] { printf("Task 1\n"); });

        td::wait_for_unpinned(s1);

        td::sync s2;
        td::submit(s2, [] {
            printf("Task 2 start \n");

            auto s2_i = td::submit_n([](auto i) { printf("Task 2 - inner %d \n", i); }, 4);

            td::wait_for_unpinned(s2_i);

            printf("Task 2 end \n");
        });

        td::submit(s2, [] { printf("Task 3 \n"); });

        td::wait_for_unpinned(s2);

        td::sync s3;
        td::submit_n(
            s3,
            [](auto i) {
                printf("Task 4 - %d start \n", i);

                auto s4_i = td::submit_n(
                    [i](auto i_inner) {
                        //
                        printf("Task 4 - %d - inner %d \n", i, i_inner);
                    },
                    4);

                printf("Task 4 - %d wait \n", i);
                td::wait_for_unpinned(s4_i);
            },
            4);

        td::wait_for_unpinned(s3);
    });


    return 0;
}
