add_executable(eblob_open eblob_open.cpp)
target_link_libraries(eblob_open eblob_cpp)

add_executable(eblob_merge merge.cpp)
target_link_libraries(eblob_merge eblob_cpp)

add_executable(eblob_index_info eblob_index_info.cpp)
target_link_libraries(eblob_index_info eblob_cpp)

install(TARGETS eblob_merge eblob_open eblob_index_info
    RUNTIME DESTINATION bin
    )
