//! Exclusive reusable workspaces for concurrent frame requests.

use std::ops::{Deref, DerefMut};
use std::sync::Mutex;

/// A lazily growing pool of reusable, exclusively checked-out workspaces.
///
/// The pool stores only idle values behind the mutex. Frame computation runs
/// through [`WorkGuard`] after checkout and therefore never holds the pool
/// lock. The factory is called only when all existing workspaces are busy, so
/// the pool grows to the highest observed host concurrency and then reuses
/// those allocations.
pub(crate) struct WorkPool<T> {
    available: Mutex<Vec<T>>,
    factory: Box<dyn Fn() -> anyhow::Result<T> + Send + Sync>,
}

impl<T: Send + 'static> WorkPool<T> {
    /// Creates a pool with one eagerly allocated workspace.
    pub(crate) fn new(
        factory: impl Fn() -> anyhow::Result<T> + Send + Sync + 'static,
    ) -> anyhow::Result<Self> {
        let first = factory()?;
        Ok(Self {
            available: Mutex::new(vec![first]),
            factory: Box::new(factory),
        })
    }

    /// Checks out an exclusive workspace, growing the pool when necessary.
    pub(crate) fn checkout(&self) -> anyhow::Result<WorkGuard<'_, T>> {
        let available = match self.available.lock() {
            Ok(mut available) => available.pop(),
            Err(poisoned) => poisoned.into_inner().pop(),
        };
        let value = match available {
            Some(value) => value,
            None => (self.factory)()?,
        };
        Ok(WorkGuard {
            pool: self,
            value: Some(value),
        })
    }
}

/// An exclusive borrow of one pooled workspace.
pub(crate) struct WorkGuard<'pool, T> {
    pool: &'pool WorkPool<T>,
    value: Option<T>,
}

impl<T> Deref for WorkGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        match self.value.as_ref() {
            Some(value) => value,
            None => unreachable!("a work guard is always initialized while borrowed"),
        }
    }
}

impl<T> DerefMut for WorkGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        match self.value.as_mut() {
            Some(value) => value,
            None => unreachable!("a work guard is always initialized while borrowed"),
        }
    }
}

impl<T> Drop for WorkGuard<'_, T> {
    fn drop(&mut self) {
        let Some(value) = self.value.take() else {
            return;
        };
        let mut available = match self.pool.available.lock() {
            Ok(available) => available,
            Err(poisoned) => poisoned.into_inner(),
        };
        available.push(value);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, atomic::AtomicUsize, atomic::Ordering};

    #[test]
    fn pool_grows_for_concurrent_checkouts_and_reuses_idle_storage() {
        let created = Arc::new(AtomicUsize::new(0));
        let factory_count = Arc::clone(&created);
        let pool = WorkPool::new(move || {
            factory_count.fetch_add(1, Ordering::Relaxed);
            Ok(Vec::<u8>::new())
        })
        .unwrap_or_else(|_| unreachable!("the test factory cannot fail"));

        let first = pool
            .checkout()
            .unwrap_or_else(|_| unreachable!("the test factory cannot fail"));
        let second = pool
            .checkout()
            .unwrap_or_else(|_| unreachable!("the test factory cannot fail"));
        assert_eq!(created.load(Ordering::Relaxed), 2);
        drop(first);
        drop(second);

        let _reused = pool
            .checkout()
            .unwrap_or_else(|_| unreachable!("the test factory cannot fail"));
        assert_eq!(created.load(Ordering::Relaxed), 2);
    }
}
