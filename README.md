# eho-server-in-c
Эхо сервер на Си

Пример работы:

Раз в `n` секунд отправляется сигнал `ALARM`, обработчик которого выводит `eho server working`.

Также тестируются команды:

```bash
$ kill -USR1 42
$ kill -INT 42
```

![](images/basic.png)

Тестируется команда:

```bash
$ kill -9 42
```

![](images/kill.png)

Выполняется следующая команда:

```bash
$ kill -QUIT 42
```

Как видно на картинке, ничего не происходит, как и должно быть по заданию.

Далее тестируется обработка сигнала `SIGTERM`.

![](images/quit_term.png)

В следующем примере тестируется закрытие терминала (сигнал `SIGHUP`).

![](images/sighup.png)

Проверяю те же сигналы, но уже в режиме демона (`-type demon`):

![](images/demon_basic.png)

Проверяю, что `quit` и `hup` игнорируются в режиме демона, а также работоспособность `term`:

![](images/demon_term_quit_hup.png)
