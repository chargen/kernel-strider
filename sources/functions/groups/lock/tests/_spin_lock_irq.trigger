[group]
function.name = _spin_lock_irq
trigger.code =>>
    DEFINE_SPINLOCK(lock);
    spin_lock_irq(&lock);
    spin_unlock_irq(&lock);
<<
