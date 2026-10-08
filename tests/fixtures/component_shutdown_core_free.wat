;; Shutdown of a defined resource with no destructor and no Core instances.
(component
  (type $resource (resource (rep i32)))
  (export "resource" (type $resource)))
