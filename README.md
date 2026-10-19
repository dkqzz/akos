# Вариант №30: Роботы на складе

Последовательная имитация автоматизированного склада на C17 для Linux.

## Сборка

```sh
./scripts/build.sh
```

Исполняемый файл находится в `build/warehouse_sim`, тесты в `build/warehouse_tests`.

## Запуск

```sh
./build/warehouse_sim --config data/demo.cfg --log demo.log --delay 0
```

Доступны параметры:

```text
--config PATH    конфигурация склада
--log PATH       файл журнала
--seed N         начальное значение генератора
--delay MS       задержка между тактами
--max-ticks N    предел числа тактов
--quiet          не выводить события и карту в терминал
--help           справка
```

Демонстрационный запуск:

```sh
./scripts/run_demo.sh
```

Тесты:

```sh
./scripts/run_tests.sh
```

## Формат конфигурации

Параметры задаются строками `ключ=значение`. Карта задаётся несколькими строками `grid=` длиной `width`. Символы карты:

| Символ | Обозначение |
| --- | --- |
| ` . ` | проход |
| `#` | препятствие/стеллаж |
| `P` | получение |
| `D` | выдача |
| `C` | зарядка |
| `N` | узкий проход |

```text
width=14
height=8
grid=##############
...
robot=1,row,col,capacity,energy,max_energy
task=101,pickup_row,pickup_col,dropoff_row,dropoff_col,weight
```

Параметры `robot` и `task` можно повторять. CLI имеет приоритет над значениями конфигурации.
