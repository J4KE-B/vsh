/* ============================================================================
 * vsh - Vanguard Shell
 * executor.h - Command execution engine
 * ============================================================================ */

#ifndef VSH_EXECUTOR_H
#define VSH_EXECUTOR_H

#include "parser.h"

typedef struct Shell Shell;

/* Execute an AST node. Returns the exit status. */
int executor_execute(Shell *shell, ASTNode *node);

/* Execute a simple command node */
int executor_exec_command(Shell *shell, CommandNode *cmd);

/* Execute a pipeline */
int executor_exec_pipeline(Shell *shell, PipelineNode *pipeline);

/* Apply redirections for the current process. Returns 0 on success. */
int executor_apply_redirections(Redirection *redirs);

/* Restore redirections (called in parent after fork) */
void executor_restore_redirections(void);

#endif /* VSH_EXECUTOR_H */
