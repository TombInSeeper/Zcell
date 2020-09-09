#ifndef _ERRCODE_H_
#define _ERRCODE_H_


enum Status {
    SUCCESS = 0,
    UNKNOWN_OP,
    INVALID_OP,
    MSGR_EAGAIN = 16,
    MSGR_CONNECT_EXCEPTION,
    OSTORE_UNSUPPORTED_OPERATION ,
    OSTORE_OBJECT_EXIST,
    OSTORE_OBJECT_NOT_EXIST,
    OSTORE_NO_SPACE,
    OSTORE_NO_NODE,
    OSTORE_WRITE_OUT_MAX_SIZE,
    OSTORE_READ_EOF,
    OSTORE_INTERNAL_UNKOWN_ERROR
};

#define OSTORE_SUBMIT_OK SUCCESS
#define OSTORE_EXECUTE_OK SUCCESS

#endif