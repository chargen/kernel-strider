<$if operation.state.value$>
	/*
     * Restriction: <$callback_name$>() should be finished in state
     * '<$operation.state.value$>'.
     */
    generate_signal(tid, pc, SELF_STATE(POST_<$operation.state.value$>)(<$operation.object$>),
        KEDR_SWT_COMMON);
<$endif$>