# How To Use

Just copy the the backend_service.c in you project, and define the following variable and function in you project. and then just compile them.

```bash
cp backend_service.c /path/to/your/project/
```

```C
vim /path/to/your/project/main.c

const char * const _SOCKET_PATH_NAME = "@backend.service.sock";

int _bg_main(int argc, char *argv[])
{
  // implement your function here
}
```

Once run the output binary, it will run the daemon and all commands will be sent to deamon for processing, and the processing results will be returned.
