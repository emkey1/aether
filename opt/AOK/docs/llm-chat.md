# LLM Chat: a built-in AI assistant with guest shell access

iSH-AOK includes an in-app LLM chat client that can talk to an
OpenAI-compatible API, Google Gemini, or (on iOS/iPadOS 26+) Apple's
on-device Foundation Models — and, optionally, run shell commands in your
guest for you.

## Enabling and opening it

It's off by default. Turn it on in Settings; once enabled, "LLM Chat"
appears both in the terminal's "Switch Terminal" menu and in the
[Workspace](workspace.md) dock.

## Providers

A "Provider" preset picker covers the common cases, or you can point it at
any OpenAI-compatible endpoint yourself:

| Preset | Notes |
|---|---|
| Apple Foundation Models | On-device, no API key, no network required (iOS/iPadOS 26+) |
| OpenRouter Free | Hosted, needs an API key |
| Groq Llama | Hosted, needs an API key |
| Gemini Flash | Uses Google's `generateContent` REST API, key passed as a query param |
| LM Studio | Local server, defaults to `127.0.0.1:1234` |
| Ollama | Local server, defaults to `127.0.0.1:11434` (this is the overall default if nothing else is configured) |
| OpenAI | Hosted, needs an API key |
| Custom | Any OpenAI-compatible chat completions endpoint |

API keys are sent as an `Authorization: Bearer` header for OpenAI-style
providers, or as a `?key=` query parameter for Gemini.

Responses stream in via Server-Sent Events where the provider supports it,
rendered incrementally with Markdown/code-fence awareness as tokens
arrive.

## Shell Tools: letting the model run commands for you

For OpenAI-compatible providers (not available with Gemini), you can grant
the assistant a `run_shell` tool. When it wants to run something — for
example, to fetch a web page via `curl` on your behalf — **you're asked to
confirm each command before it executes.** Command output is capped at
64 KB and the command is killed if it runs longer than 30 seconds.

Nothing runs without your explicit per-command confirmation; there's no
"auto-approve everything" mode.

## Persistence

Chat history is saved to `/AOK/persist/llm-chat.json`, so it survives root
switches and app restarts. Saved extracts and prompt templates live under
`/AOK/persist/llm-extracts` and `/AOK/persist/llm-prompts` respectively —
see [persist.md](persist.md).
