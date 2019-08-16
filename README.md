# lua-debugger
--- 
[中文](README_zh.md)

Lua-debugger makes lua virtual machine capable of single-step debugging by
adding a debugging server. Instead of using Lua's hook machanism, we created a new OP_INTERRUPT instruction, and inject it into the proper position of the instruction list when a breakpoint is set. 


## How to use

1. Download lua 5.3.x source code.
2. Replace original lua source files with files in directoy src.
3. Recompile lua(make linux). 

To enable the function of debugging, it is recommended that first all your modules, and then invoke 
`debug.startserver()`
then, when the application starts, the screen may look like this:
```
Lua VM paused at print.lua:21
    19  end
    20
->  21  debug.startserver()
    22  foo()
    23
    24
```
It means the virtual machine has been paused at startserver(), and then you can start debugging with the supplied commands.




## Lua Functions

#### debug.startserver(mode='i', addr='0.0.0.0')
Start the debugging server.

###### Parameters:
* `mode`: 'i' for interactive mode, 'f' for foreground mode, 'b' for background mode.
* `addr`: listen IP address.
 

The server can be run in 3 different modes:
* #####Interactive mode 
The virtual machine will be paused and commands will be received and processed through console(stdin and stdout).

* ##### Foregroud mode 
Similar to interactive mode, but will listen on port 7609 and process commands thought TCP connection. Whenever the connection is broken, the lua program will exit.

* ##### Backgroup mode
Listen on port 7609 without pausing the virtual machine. Debugger client connects it, sends command `pause` to explicitly pause the virtual machine. If the connection is broken, all breakpoints will be removed and the virtual machine continues. 

###### Returns:
0 if succeeds, otherwise an errno defined by POSIX.  


#### debug.pause()
Pause the virtual machine. This is a light-weight solution for conditional breakpoints, like this:
```
for i = 1, 1000000 do 
	if i == 654321 then
		debug.pause()
	end
end
```

## Debugger Commands

### `<return>`
Repeat the previous command.

### pause (pa)
Pause the Lua virutal machine. 
This command can only be used in backgroup mode, and only when the virtual machine is running.

### continue (c)
Continue the lua virtual machine.

### list (l)
List source code.
```
> list frame.lua 10 
    10  local function f3()
    11          f2()
    12  end
    13
->  14  debug.startserver()
    15  f3()
    16
    17
> list
<EOF> 
> list frame.lua 
     1
     2  local function f1()
     3          x = x + 1
     4  end
     5
     6  local function f2()
     7          f1()
     8  end
     9
    10  local function f3()
```

### print (p)
Print variables' values.
```
> print i1 i2
i1 = 10
i2 = 20
> print b1 b2
b1 = true
b2 = false
> print s
s = 'string'
> print t 
t = (0x555b3b8f3170, sizearray=0, sizenode=4){
--node
        ['a'] = 1,
        ['b'] = 2,
        ['c'] = (0x555b3b8f1a70, sizearray=0, sizenode=4){},
}
> print t.c
t.c = (0x555b3b8f1a70, sizearray=0, sizenode=4){
--node
        ['cc'] = 3,
        ['bb'] = 2,
        ['aa'] = 1,
}
> print foo
foo = function
```

### info (i)
Show information about breakpoints|arguments|local-variables|up-values.
info breaks|args|locals|upvals


### break (b)
Set a breakpoint.
```
> break 10
breakpoint #1 set at break.lua:10
> break other.lua 20
breakpoint #2 set at other.lua:10
```

### tb (tb)
Set a breakpoint which will only be triggered once.


### enable (ea)
Enable breakpoints(or other objects, in the future).
```
> enable breaks 1 2 
disabled 2 breakpoint(s)
> enable breaks
disabled 3 breakpoint(s)
```

### disable (da)
Disable breakpoints(or other objects, in the future).
```
> disable breaks 1 2 
disabled 2 breakpoint(s)
> disable breaks
disabled 3 breakpoint(s)
```

### delete (d)
Delete breakpoints(or other objects, in the future).
```
> delete breaks 1 2 
deleted 2 breakpoint(s)
> disable breaks
deleted 3 breakpoint(s)
```


### backtrace (bt)
Show the stack.
```
> backtrace
->  frame.lua:3: in upvalue 'f1'
    frame.lua:7: in upvalue 'f2'
    frame.lua:11: in local 'f3'
    frame.lua:15: in main chunk
    [C]: in ?
```


### frame (f)
Switch the current debugging frame.
```
> frame 1
in "frame.lua":
     5
     6  local function f2()
->   7          f1()
     8  end
     9
    10  local function f3()
    11          f2()
    12  end
    13
    14  debug.startserver()
> frame 4
unable to enter C-frame
```

### next (n)
Step forward by one line (skipping over functions).

### step (s)
Step forward by one line (into functions).


### finish (fi)
Finish the current call.

### until (un)
Keep running until jump out of the current loop.

### quit (q)
Quit the debugging.


## Known Issues:
* Currently it can only be run on *nix systems.