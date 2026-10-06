# Public Source Notice

For anyone who may be interested, we have agreed to open the source files for the main SteamVita launcher, which currently also acts as the game installer.

At this time, we are **not comfortable releasing the compatibility/runtime files**. Internal testing has caused hard crashes on real PlayStation Vita systems, along with other issues that we have not fully identified or resolved yet.

Because of that, the public launcher source intentionally blocks game launching and replaces the private compatibility hooks with disabled placeholders. The launcher and installer code can still be reviewed and worked on without exposing an unfinished runtime that may cause instability or hardware crashes.

We plan to keep the compatibility layer private until we are more confident that it is safe enough for wider testing.
