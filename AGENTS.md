# Agent Instructions

## ⚡ Superpowers — Mandatory Skill System

**You MUST invoke the `using-superpowers` skill at the start of every conversation involving coding, debugging, or agentic development work, before any other action.**

- **Slash Command / Skill:** Always invoke `/using-superpowers` at the beginning of tasks.
- **Skill Path:** `%USERPROFILE%\.gemini\config\plugins\superpowers\skills\using-superpowers\SKILL.md`
- **Strict Requirement:** Non-negotiable for coding, debugging, or building features. No exceptions — invoke relevant or requested skills BEFORE any response or action (including clarifying questions, exploring codebase, or checking files).

---

## 🛠️ Skill Workflow & Priority

After invoking `using-superpowers`, check for and invoke any applicable sub-skills before proceeding:

| Situation | Skill to invoke |
| :--- | :--- |
| Any creative work, new feature, or building something | `brainstorming` |
| Any bug, test failure, or unexpected behavior | `systematic-debugging` |
| Implementing a feature or bugfix | `test-driven-development` |
| Multi-step task with spec or requirements | `writing-plans` |
| Approved plan ready to execute (separate session) | `executing-plans` |
| Complex implementation with independent tasks | `subagent-driven-development` |
| Multiple independent tasks (2+) | `dispatching-parallel-agents` |
| Work complete, need to verify before claiming done | `verification-before-completion` |
| Work complete, deciding how to integrate | `finishing-a-development-branch` |

---

## 🚫 Red Flags — Stop and Check for a Skill

If you catch yourself thinking:
- "This is just a simple question / task"
- "I need more context or need to explore files first"
- "This doesn't need a formal skill"
- "The skill is overkill"

**STOP immediately and invoke the required skill first.**
