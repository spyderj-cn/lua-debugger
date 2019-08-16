# lua-debugger
--- 
[English](README.md)

Lua-debugger 在lua引擎中嵌入了一个调试服务器，使得lua引擎在运行时支持单步调试。大多数lua调试器使用hook机制实现断点，而lua-debugger则是根据断点位置将相应的lua指令替换成OP_INTERRUPT，当引擎执行到此指令时自动暂停。这种断点机制可以让lua引擎全速运行，效率极高。 


## 使用方式

1. 下载lua 5.3.x源代码（建议5.3.4, 5.3.5）。
2. 将src/目录下的文件替换掉lua的源代码文件。
3. 重新编译lua。

生成的新lua程序即自带单步调试功能。
要启用调试服务器，只需要在代码中增加一行`debug.startserver()`即可，当程序运行后控制台如下：
```
Lua VM paused at print.lua:21
    19  end
    20
->  21  debug.startserver()
    22  foo()
    23
    24
```
表示当前虚拟机已经暂停，等待用户的调试命令。

调试服务器只能追踪迄今为止已加载的模块，因此一般来说应该在载入了所有模块后再调用`debug.startserver()`。


## Lua 函数

#### debug.startserver(mode='i', addr='0.0.0.0')
启动调试服务器。

###### 参数:
* `mode`: 'i'为控制台交互模式, 'f'为前台模式, 'b'为后台模式。
* `addr`: 侦听的IP地址。
 

调试服务器可以3种不同的模式运行：

* #####控制台交互模式
lua程序运行后将自动暂停在debug.startserver()，调试服务器直接从控制台读取调试指令，并将结果输出到控制台。 

* ##### 前台模式
lua程序运行后将自动暂停在debug.startserver()，调试服务器侦听在7609端口，并等待客户端的连接，然后从连接读取调试命令并将结果输出到该连接。连接断开后程序自动退出。

* ##### 后台模式
lua程序保持运行状态，调试服务器侦听在7609端口，并等待客户端的连接，然后从连接读取调试命令并将结果输出到该连接。客户端连接后需显式地发送`pause`命令暂停lua引擎。当连接断开后调试服务器将清空所有断点并继续运行lua引擎。 

###### 返回:
成功返回0， 错误时返回一个POSIX定义的errno。
  


#### debug.pause()
暂停lua虚拟机。可以用来实现条件断点，比如：
```
for i = 1, 1000000 do 
	if i == 654321 then
		debug.pause()
	end
end
```

## 调试命令

### `<return>`
直接回车为重复执行上一条命令。

### pause (pa)
暂停lua虚拟机。
这条命令只能用于后台模式，并且只能在lua虚拟机正在运行的状态下使用。

### continue (c)
让lua虚拟机继续运行。

### list (l)
列出源代码。
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
打印变量值。
string|number|boolean|nil 会打印值。
table只会展开一层，如果要打印深层数据可指定字段，比如`print t.level1.level2[1].level3`。
其他类型打印出类型名称。
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
显示(断点|参数|局部变量|upvalues)的信息。
info breaks|args|locals|upvals


### break (b)
设置断点。
```
> break 10
breakpoint #1 set at break.lua:10
> break other.lua 20
breakpoint #2 set at other.lua:10
```

### tb (tb)
设置一个临时断点。临时断点触发一次后自动删除。


### enable (ea)
启用断点（将来可能有更多支持）。
```
> enable breaks 1 2 
disabled 2 breakpoint(s)
> enable breaks
disabled 3 breakpoint(s)
```

### disable (da)
禁用断点（将来可能有更多支持）。
```
> disable breaks 1 2 
disabled 2 breakpoint(s)
> disable breaks
disabled 3 breakpoint(s)
```

### delete (d)
删除断点（将来可能有更多支持）。
```
> delete breaks 1 2 
deleted 2 breakpoint(s)
> disable breaks
deleted 3 breakpoint(s)
```


### backtrace (bt)
输出栈信息。
```
> backtrace
->  frame.lua:3: in upvalue 'f1'
    frame.lua:7: in upvalue 'f2'
    frame.lua:11: in local 'f3'
    frame.lua:15: in main chunk
    [C]: in ?
```


### frame (f)
切换栈帧。 最上层的为0。无法切换到C帧。
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
运行到下一行代码（跳过函数调用）。

### step (s)
运行到下一行代码（进入函数调用）。


### finish (fi)
执行完当前函数。

### until (un)
执行完当前循环。

### quit (q)
退出调试。


## 已知问题
* 当前只支持linux系统（只在linux系统上使用过）。