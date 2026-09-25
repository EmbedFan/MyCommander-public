# ASCII / IBM PC Frame Characters

If by **"ASCII PC characters"** you mean the classic **IBM PC/DOS
box-drawing characters** from **Code Page 437 (CP437)**, these are the
characters traditionally used to make text-mode frames, boxes, menus,
and tables.

## Single-line frames

``` text
┌──────────────┐
│              │
│              │
└──────────────┘
```

  Character     CP437 decimal Purpose
  ----------- --------------- -----------------
  `─`                     196 horizontal
  `│`                     179 vertical
  `┌`                     218 top-left
  `┐`                     191 top-right
  `└`                     192 bottom-left
  `┘`                     217 bottom-right
  `├`                     195 left junction
  `┤`                     180 right junction
  `┬`                     194 top junction
  `┴`                     193 bottom junction
  `┼`                     197 crossing

## Double-line frames

``` text
╔══════════════╗
║              ║
║              ║
╚══════════════╝
```

  Character     CP437 decimal
  ----------- ---------------
  `═`                     205
  `║`                     186
  `╔`                     201
  `╗`                     187
  `╚`                     200
  `╝`                     188
  `╠`                     204
  `╣`                     185
  `╦`                     203
  `╩`                     202
  `╬`                     206

## Mixed single/double-line characters

``` text
╒═╕   ╓─╖
│ │   ║ ║
╘═╛   ╙─╜
```

These include:

`╒ ╓ ╕ ╖ ╘ ╙ ╛ ╜ ╞ ╟ ╡ ╢ ╤ ╥ ╧ ╨ ╪ ╫`

## True 7-bit ASCII

Strictly speaking, the IBM PC box-drawing characters above are **not
ASCII**. ASCII only covers character codes **0--127**. They are
characters from the extended IBM PC character set, most commonly
**CP437**.

If you need **true 7-bit ASCII only**, frames are normally made with
`+`, `-`, and `|`:

``` text
+----------------+
|                |
|                |
+----------------+
```
