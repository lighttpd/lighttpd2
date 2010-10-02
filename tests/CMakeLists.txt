
cmake_policy(VERSION 2.6.4)

add_test(NAME http COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/runtests.py --angel $<TARGET_FILE:lighttpd2> --worker $<TARGET_FILE:lighttpd2-worker> --plugindir $<TARGET_FILE_DIR:lighttpd2>)
