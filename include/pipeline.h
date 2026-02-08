/* ============================================================================
 * vsh - Vanguard Shell
 * pipeline.h - Pipe chain management
 * ============================================================================ */

#ifndef VSH_PIPELINE_H
#define VSH_PIPELINE_H

#include <sys/types.h>

typedef struct Shell Shell;
typedef struct ASTNode ASTNode;
typedef struct PipelineNode PipelineNode;

/* Execute a pipeline of commands connected by pipes.
 * Returns the exit status of the last command in the pipeline. */
int pipeline_execute(Shell *shell, PipelineNode *pipeline);

#endif /* VSH_PIPELINE_H */
